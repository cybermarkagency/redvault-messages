// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import android.util.Base64
import com.redvaultsystems.redvaultmessage.devicekeys.KeyWrapper
import java.io.File
import java.security.MessageDigest

/**
 * Keeps the server's trusted-device token (cookie rv_device) for each handle across app restarts. With it, strangers'
 * wrong passwords can't lock this phone out of the account (server: db/migrations/052_login_devices.sql,
 * security assessment 2026-11-15 RV-1115-01). Without it, sign-in works exactly as before.
 */
interface LoginDeviceTokens {
    fun load(handle: String): String?
    fun save(handle: String, token: String)
}

object NoLoginDeviceTokens : LoginDeviceTokens {
    override fun load(handle: String): String? = null
    override fun save(handle: String, token: String) = Unit
}

/**
 * The token is a secret, so it is kept wrapped by an Android Keystore key (the same [KeyWrapper] as the device keys)
 * in noBackupFilesDir: never in a cloud backup or a device-to-device transfer. File names are a hash of the handle, so
 * no account name is written to disk. Any failure only costs the lockout bypass, never the sign-in.
 */
class WrappedLoginDeviceTokens(
    private val dir: File,
    private val wrapper: KeyWrapper,
    private val encode: (ByteArray) -> String = { Base64.encodeToString(it, Base64.NO_WRAP) },
    private val decode: (String) -> ByteArray = { Base64.decode(it, Base64.NO_WRAP) },
) : LoginDeviceTokens {
    private fun id(handle: String): String =
        MessageDigest.getInstance("SHA-256").digest(handle.lowercase().toByteArray()).joinToString("") { "%02x".format(it) }

    private fun file(handle: String) = File(dir, "rv-device-${id(handle)}.txt")

    private fun aad(handle: String) = "redvault-login-device\u0000${handle.lowercase()}".toByteArray()

    override fun load(handle: String): String? = runCatching {
        val lines = file(handle).takeIf { it.isFile }?.readLines() ?: return null
        if (lines.size < 2) return null
        String(wrapper.unwrap(ALIAS, KeyWrapper.Wrapped(decode(lines[0]), decode(lines[1])), aad(handle)))
            .takeIf { it.isNotEmpty() && it.length <= 256 }
    }.getOrNull()

    @Synchronized
    override fun save(handle: String, token: String) {
        runCatching {
            dir.mkdirs()
            val wrapped = wrapper.wrap(ALIAS, token.toByteArray(), aad(handle))
            val target = file(handle)
            val temp = File(dir, target.name + ".tmp")
            temp.writeText(encode(wrapped.iv) + "\n" + encode(wrapped.ciphertext) + "\n")
            if (!temp.renameTo(target)) {
                target.delete()
                temp.renameTo(target)
            }
        }
    }

    companion object {
        const val ALIAS = "redvault.login-device"
    }
}
