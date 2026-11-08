// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import java.security.MessageDigest

/** How long the app may sit in the background before it asks for the PIN again. */
enum class LockTimeout(val millis: Long, val label: String) {
    Immediately(0, "Immediately"),
    ThirtySeconds(30_000, "After 30 seconds"),
    OneMinute(60_000, "After 1 minute"),
    FiveMinutes(5 * 60_000, "After 5 minutes"),
    FifteenMinutes(15 * 60_000, "After 15 minutes"),
}

/**
 * The app-lock PIN, stored only as an Argon2id hash with a random salt (the
 * same KDF and cost as the password), never the PIN itself. Too many wrong
 * attempts end the session, which wipes the keys from memory; getting back
 * in then takes the account password.
 */
class AppLockStore(context: Context, private val crypto: E2eCrypto) {
    private val prefs = context.getSharedPreferences("appLock", Context.MODE_PRIVATE)

    val enabled: Boolean get() = prefs.contains(KEY_HASH)

    /** Lets the lock screen show one dot per digit and submit on the last one. */
    val pinLength: Int get() = prefs.getInt(KEY_LENGTH, MIN_PIN_LENGTH)

    var timeout: LockTimeout
        get() = LockTimeout.entries.firstOrNull { it.name == prefs.getString(KEY_TIMEOUT, null) } ?: LockTimeout.OneMinute
        set(value) = prefs.edit().putString(KEY_TIMEOUT, value.name).apply()

    val failedAttempts: Int get() = prefs.getInt(KEY_FAILED, 0)

    /** Hashing is deliberately slow (Argon2id); call off the main thread. */
    fun setPin(pin: String) {
        require(isValidPin(pin)) { "PIN must be $MIN_PIN_LENGTH to $MAX_PIN_LENGTH digits" }
        val salt = crypto.randomBytes(E2eCrypto.PWHASH_SALT_BYTES)
        prefs.edit()
            .putString(KEY_HASH, Base64Codec.encode(hash(pin, salt)))
            .putString(KEY_SALT, Base64Codec.encode(salt))
            .putInt(KEY_LENGTH, pin.length)
            .putInt(KEY_FAILED, 0)
            .apply()
    }

    /** Checks a PIN, counting failures. Call off the main thread. */
    fun verify(pin: String): Boolean {
        val stored = prefs.getString(KEY_HASH, null) ?: return false
        val salt = prefs.getString(KEY_SALT, null) ?: return false
        val ok = MessageDigest.isEqual(hash(pin, Base64Codec.decode(salt)), Base64Codec.decode(stored))
        prefs.edit().putInt(KEY_FAILED, if (ok) 0 else failedAttempts + 1).apply()
        return ok
    }

    fun clear() {
        prefs.edit().remove(KEY_HASH).remove(KEY_SALT).remove(KEY_LENGTH).remove(KEY_FAILED).apply()
    }

    private fun hash(pin: String, salt: ByteArray): ByteArray = crypto.deriveWrapKey(pin, salt)

    companion object {
        const val MIN_PIN_LENGTH = 4
        const val MAX_PIN_LENGTH = 12
        /** After this many wrong PINs in a row the session ends. */
        const val MAX_ATTEMPTS = 10

        fun isValidPin(pin: String) = pin.length in MIN_PIN_LENGTH..MAX_PIN_LENGTH && pin.all(Char::isDigit)

        private const val KEY_HASH = "pinHash"
        private const val KEY_SALT = "pinSalt"
        private const val KEY_LENGTH = "pinLength"
        private const val KEY_TIMEOUT = "timeout"
        private const val KEY_FAILED = "failedAttempts"
    }
}
