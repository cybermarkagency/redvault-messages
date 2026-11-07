package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec

/**
 * Trust on first use, like the desktop's prefs::hasKeyChanged: remembers the
 * first identity key seen for each contact, and says whether the key now
 * differs. A changed key is never accepted silently; it stays flagged until
 * the new safety number is verified ([accept]). Kept on this phone only.
 */
class ContactKeyPins(private val storage: Storage) {
    /** Where pins live: SharedPreferences on the phone, a map in tests. */
    interface Storage {
        fun get(name: String): String?
        fun put(name: String, value: String)
    }

    /**
     * Whether [contactUserId]'s key differs from the one first seen here; the
     * first key seen is pinned. A key this phone verified is trusted as is, and
     * pinned too, so a later change is caught even when it was verified before
     * pins existed (verified marks are per key, and would just go quiet).
     */
    fun keyChanged(selfUserId: String, contactUserId: String, publicKeyB64: String, verified: Boolean): Boolean {
        if (publicKeyB64.isEmpty()) return false
        val name = name(selfUserId, contactUserId)
        val current = normalize(publicKeyB64)
        val known = storage.get(name)
        if (verified || known == null) {
            if (known != current) storage.put(name, current)
            return false
        }
        return known != current
    }

    /** Pins [publicKeyB64] as [contactUserId]'s key: verifying the new safety number is what clears a change. */
    fun accept(selfUserId: String, contactUserId: String, publicKeyB64: String) {
        if (publicKeyB64.isEmpty()) return
        storage.put(name(selfUserId, contactUserId), normalize(publicKeyB64))
    }

    private fun name(selfUserId: String, contactUserId: String) = "$selfUserId:$contactUserId"

    // Compared as bytes (the desktop compares hex), so a differently padded or wrapped copy isn't a "change".
    private fun normalize(keyB64: String): String =
        runCatching { Base64Codec.encode(Base64Codec.decode(keyB64.trim())) }.getOrDefault(keyB64)
}

/** [ContactKeyPins] storage in this phone's SharedPreferences. */
class ContactKeyPinPrefs(context: Context) : ContactKeyPins.Storage {
    private val prefs = context.getSharedPreferences("contactKeyPins", Context.MODE_PRIVATE)

    override fun get(name: String): String? = prefs.getString(name, null)

    override fun put(name: String, value: String) = prefs.edit().putString(name, value).apply()
}
