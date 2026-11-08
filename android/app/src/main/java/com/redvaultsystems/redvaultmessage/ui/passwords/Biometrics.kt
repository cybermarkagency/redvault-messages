// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.passwords

import android.content.Context
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.Build
import android.os.CancellationSignal

/**
 * Unlocking the password vault with the phone's fingerprint or face, through
 * the system prompt (Android 9+; older phones use the account password).
 * The biometric only proves the owner is holding the phone: the vault key
 * itself is already in memory for this session, exactly as on the desktop.
 */
object Biometrics {
    fun available(context: Context): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val manager = context.getSystemService(BiometricManager::class.java) ?: return false
            return manager.canAuthenticate(BiometricManager.Authenticators.BIOMETRIC_WEAK) == BiometricManager.BIOMETRIC_SUCCESS
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            @Suppress("DEPRECATION")
            val manager = context.getSystemService(BiometricManager::class.java) ?: return false
            @Suppress("DEPRECATION")
            return manager.canAuthenticate() == BiometricManager.BIOMETRIC_SUCCESS
        }
        return context.packageManager.hasSystemFeature("android.hardware.fingerprint")
    }

    /** Shows the system prompt; [onResult] gets null on success, or a message (null message and false = cancelled). */
    fun prompt(context: Context, onResult: (success: Boolean, error: String?) -> Unit) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return onResult(false, "This phone can't unlock with a fingerprint here. Use your password.")
        val builder = BiometricPrompt.Builder(context)
            .setTitle("Unlock your password vault")
            .setSubtitle("Confirm it's you")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            builder.setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_WEAK)
        }
        builder.setNegativeButton("Use password", context.mainExecutor) { _, _ -> onResult(false, null) }
        val prompt = builder.build()
        prompt.authenticate(
            CancellationSignal(),
            context.mainExecutor,
            object : BiometricPrompt.AuthenticationCallback() {
                override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult?) = onResult(true, null)

                override fun onAuthenticationError(errorCode: Int, errString: CharSequence?) {
                    val cancelled = errorCode == BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED || errorCode == BiometricPrompt.BIOMETRIC_ERROR_CANCELED
                    onResult(false, if (cancelled) null else errString?.toString())
                }
            },
        )
    }
}
