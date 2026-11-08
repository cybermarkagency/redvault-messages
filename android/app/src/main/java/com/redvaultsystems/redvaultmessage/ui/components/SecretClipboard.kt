// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.components

import android.content.ClipData
import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.Context
import android.os.Build
import android.os.PersistableBundle
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Copies to the clipboard. Secrets (passwords, codes, raw keys) are marked
 * sensitive, so Android hides them in its clipboard preview, and are
 * cleared after 30 seconds. The timer belongs to the process, not the
 * screen, so leaving the screen doesn't keep the secret on the clipboard.
 */
object SecretClipboard {
    const val CLEAR_AFTER_MS = 30_000L

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var pending: Job? = null
    private var copiedSecret: String? = null

    fun copy(context: Context, text: String, secret: Boolean) {
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = ClipData.newPlainText(if (secret) "Secret" else "Text", text)
        if (secret) markSensitive(clip)
        clipboard.setPrimaryClip(clip)
        pending?.cancel()
        if (!secret) {
            copiedSecret = null
            return
        }
        copiedSecret = text
        val app = context.applicationContext
        pending = scope.launch {
            delay(CLEAR_AFTER_MS)
            clearIfStillOurs(app)
        }
    }

    /**
     * Private text the user chose to copy (a message): marked sensitive, so Android hides it in its clipboard
     * preview, but not cleared after 30 seconds; it's the user's to paste.
     */
    fun copyPrivate(context: Context, text: String) {
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = ClipData.newPlainText("Text", text)
        markSensitive(clip)
        clipboard.setPrimaryClip(clip)
        pending?.cancel()
        copiedSecret = null
    }

    /** The flag is API 33's, but keyboards and clipboard apps on older versions look for the same key. */
    private fun markSensitive(clip: ClipData) {
        clip.description.extras = PersistableBundle().apply { putBoolean(ClipDescription.EXTRA_IS_SENSITIVE, true) }
    }

    /** Clears a secret we copied, now (signing out, locking the vault). Never touches what the user copied since. */
    fun clearNow(context: Context) {
        pending?.cancel()
        clearIfStillOurs(context.applicationContext)
    }

    private fun clearIfStillOurs(context: Context) {
        val secret = copiedSecret ?: return
        copiedSecret = null
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        // In the background Android 10+ won't let us read the clipboard (null): clear it anyway,
        // since leaving a password there is worse than dropping something copied in the last 30 s.
        val current = runCatching { clipboard.primaryClip?.getItemAt(0)?.text?.toString() }.getOrNull()
        if (current == null || current == secret) runCatching { clear(clipboard) }
    }

    /** clearPrimaryClip is API 28. Android 8 (API 26-27) had none, so the secret is overwritten with an empty clip there. */
    private fun clear(clipboard: ClipboardManager) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) clipboard.clearPrimaryClip()
        else clipboard.setPrimaryClip(ClipData.newPlainText("", ""))
    }
}
