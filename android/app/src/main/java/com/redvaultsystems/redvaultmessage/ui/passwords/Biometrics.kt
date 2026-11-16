// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.passwords

import android.content.Context
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.Build
import android.os.CancellationSignal
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import java.security.KeyStore
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey

/**
 * Unlocking the password vault with the phone's fingerprint or face, through
 * the system prompt (Android 9+; older phones use the account password).
 *
 * What it proves (security assessment 2026-11-15, RV-1115-14):
 *  - Only strong (class 3) biometrics count; weaker face unlock is refused.
 *  - The unlock is checked by the phone's secure hardware, not just by a
 *    callback: success needs an Android Keystore key that works only right
 *    after a biometric check ([CryptoObject][BiometricPrompt.CryptoObject]).
 *  - That key is destroyed when the phone's fingerprints or faces change, and
 *    is only made again after the account password unlocks the vault
 *    ([enroll]), so adding your own fingerprint to someone's phone doesn't
 *    open their vault.
 *
 * What it doesn't change: the account's private key (which also opens the
 * vault's items) stays in memory while signed in, as chats need it, exactly as
 * on the desktop. The lock keeps the items off the screen and out of memory;
 * it isn't a second encryption layer against code running inside the app.
 */
object Biometrics {
    private const val KEY_ALIAS = "redvault.vault-biometric"

    /** Whether the phone can do a strong biometric check at all. */
    fun hardwareAvailable(context: Context): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val manager = context.getSystemService(BiometricManager::class.java) ?: return false
            return manager.canAuthenticate(BiometricManager.Authenticators.BIOMETRIC_STRONG) == BiometricManager.BIOMETRIC_SUCCESS
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            // Android 10's BiometricPrompt only ever uses strong biometrics.
            @Suppress("DEPRECATION")
            val manager = context.getSystemService(BiometricManager::class.java) ?: return false
            @Suppress("DEPRECATION")
            return manager.canAuthenticate() == BiometricManager.BIOMETRIC_SUCCESS
        }
        return context.packageManager.hasSystemFeature("android.hardware.fingerprint")
    }

    /** Whether the vault can be unlocked by biometrics now: strong hardware and a key made after a password unlock. */
    fun available(context: Context): Boolean = hardwareAvailable(context) && runCatching { key() != null }.getOrDefault(false)

    /**
     * Makes (or remakes) the biometric key. Call only right after the account password was checked: at sign-in and
     * after a password unlock of the vault. Fails quietly when the phone has no enrolled strong biometric.
     */
    fun enroll(context: Context) {
        if (!hardwareAvailable(context)) return
        runCatching {
            forget()
            val builder = KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .setUserAuthenticationRequired(true)
                .setInvalidatedByBiometricEnrollment(true)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                // Every use needs a fresh strong biometric check; the screen lock PIN doesn't count.
                builder.setUserAuthenticationParameters(0, KeyProperties.AUTH_BIOMETRIC_STRONG)
            } else {
                @Suppress("DEPRECATION")
                builder.setUserAuthenticationValidityDurationSeconds(-1)
            }
            KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
                init(builder.build())
                generateKey()
            }
        }
    }

    /** Removes the key (sign-out, or the phone's biometrics changed). */
    fun forget() {
        runCatching { keyStore().deleteEntry(KEY_ALIAS) }
    }

    private fun keyStore(): KeyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }

    private fun key(): SecretKey? = keyStore().getKey(KEY_ALIAS, null) as SecretKey?

    /**
     * Shows the system prompt. [onResult] gets (true, null) only when the secure hardware released the key after a
     * strong biometric; (false, null) when cancelled or "Use password" was tapped; (false, message) otherwise.
     */
    fun prompt(context: Context, onResult: (success: Boolean, error: String?) -> Unit) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return onResult(false, "This phone can't unlock with a fingerprint here. Use your password.")
        val cipher = try {
            val key = key() ?: return onResult(false, "Unlock with your password once to use your fingerprint or face.")
            Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key) }
        } catch (_: KeyPermanentlyInvalidatedException) {
            forget()
            return onResult(false, "The fingerprints or faces on this phone changed. Unlock with your password.")
        } catch (_: Exception) {
            return onResult(false, "Fingerprint unlock isn't available right now. Use your password.")
        }
        val builder = BiometricPrompt.Builder(context)
            .setTitle("Unlock your password vault")
            .setSubtitle("Confirm it's you")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            builder.setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_STRONG)
        }
        builder.setNegativeButton("Use password", context.mainExecutor) { _, _ -> onResult(false, null) }
        builder.build().authenticate(
            BiometricPrompt.CryptoObject(cipher),
            CancellationSignal(),
            context.mainExecutor,
            object : BiometricPrompt.AuthenticationCallback() {
                override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult?) {
                    // The proof: the key only encrypts if the secure hardware just saw a strong biometric.
                    val released = runCatching {
                        val unlocked = result?.cryptoObject?.cipher ?: return@runCatching false
                        unlocked.doFinal(ByteArray(16).also(SecureRandom()::nextBytes)).isNotEmpty()
                    }.getOrDefault(false)
                    if (released) onResult(true, null) else onResult(false, "That didn't unlock the vault. Use your password.")
                }

                override fun onAuthenticationError(errorCode: Int, errString: CharSequence?) {
                    val cancelled = errorCode == BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED || errorCode == BiometricPrompt.BIOMETRIC_ERROR_CANCELED
                    onResult(false, if (cancelled) null else errString?.toString())
                }
            },
        )
    }
}
