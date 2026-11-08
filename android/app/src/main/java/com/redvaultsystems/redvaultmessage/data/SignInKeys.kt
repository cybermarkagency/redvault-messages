// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.KdfSettings
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.PreLoginRequest
import com.redvaultsystems.redvaultmessage.data.api.PreLoginResult
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import java.util.concurrent.ConcurrentHashMap

/** Which accounts this phone saw on sign-in keys: from then on it refuses to send the password for them. */
interface SignInPins {
    fun isPinned(handle: String): Boolean
    fun pin(handle: String)
}

class PreferenceSignInPins(context: Context) : SignInPins {
    private val prefs = context.getSharedPreferences("signInKeys", Context.MODE_PRIVATE)

    override fun isPinned(handle: String): Boolean = prefs.getBoolean(handle, false)

    override fun pin(handle: String) {
        prefs.edit().putBoolean(handle, true).apply()
    }
}

class MemorySignInPins : SignInPins {
    private val handles = ConcurrentHashMap.newKeySet<String>()

    override fun isPinned(handle: String): Boolean = handle in handles

    override fun pin(handle: String) {
        handles.add(handle)
    }
}

/** The settings a server names, or the ones every account had before settings were stored. */
internal fun kdfSettings(ops: Long?, mem: Long?): KdfSettings =
    if (ops != null && mem != null) KdfSettings(ops, mem) else E2eCrypto.KDF_INTERACTIVE

/** What to send to prove a password. */
data class Credential(val password: String, val authVersion: Int? = null, val upgradeAuthKey: String? = null)

/**
 * Signing in without giving the server the password (docs/auth-key-split.md).
 * Every password check goes through here, so no path can send it by accident.
 */
class SignInKeys(private val api: ApiService, private val crypto: E2eCrypto, private val pins: SignInPins) {
    /** At sign-in, or when an action re-checks the password. */
    suspend fun credentialFor(handle: String, password: String): Credential {
        val pre = preLogin(handle)
        // Throws UnsafeKdfException before anything is derived or sent, if the server asks for settings out of range.
        val signInKey = crypto.deriveSignInKey(password, Base64Codec.decode(pre.kdfSalt), kdfSettings(pre.kdfOps, pre.kdfMem))
        if (pre.authVersion == 2) return Credential(signInKey, authVersion = 2)
        refuseIfPinned(handle)
        return if (pre.upgrade) Credential(password, upgradeAuthKey = signInKey) else Credential(password)
    }

    /** Just the value, for a re-check while signed in. */
    suspend fun passwordProof(handle: String, password: String): String = credentialFor(handle, password).password

    /** For a new password wrapped with [kdfSalt]: at sign-up, password change and recovery. */
    suspend fun newCredential(handle: String, password: String, kdfSalt: ByteArray): Credential {
        val pre = preLogin(handle)
        if (pre.authVersion == 2 || pre.upgrade) return Credential(crypto.deriveSignInKey(password, kdfSalt, E2eCrypto.KDF_CURRENT), authVersion = 2)
        refuseIfPinned(handle)
        return Credential(password)
    }

    fun pin(handle: String) = pins.pin(pinName(handle))

    /** The account has a new name: the pin goes with it. */
    fun movePin(oldHandle: String, newHandle: String) {
        if (pins.isPinned(pinName(oldHandle))) pin(newHandle)
    }

    /** The server ignores case in usernames, so a pin has to as well. */
    private fun pinName(handle: String) = handle.trim().lowercase()

    private suspend fun preLogin(handle: String): PreLoginResult =
        apiCall(ApiClient.json) { api.preLogin(PreLoginRequest(handle)) }

    private fun refuseIfPinned(handle: String) {
        if (pins.isPinned(pinName(handle))) throw UserFacingException(E2eCrypto.SIGN_IN_DETAILS_WRONG)
    }
}
