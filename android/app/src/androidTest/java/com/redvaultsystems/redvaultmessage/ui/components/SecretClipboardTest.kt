package com.redvaultsystems.redvaultmessage.ui.components

import android.content.ClipboardManager
import android.content.Context
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test
import org.junit.runner.RunWith

/**
 * A copied secret really leaves the clipboard, on every supported Android version. On Android 8 (API 26-27)
 * clearPrimaryClip doesn't exist, and the secret used to stay (2026-11-04 assessment, F-30).
 */
@RunWith(AndroidJUnit4::class)
class SecretClipboardTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context: Context = instrumentation.targetContext

    /** The clipboard's text now, read on the main thread (Android 8's ClipboardManager needs a Looper). */
    private fun clipboardText(): String? {
        var text: String? = null
        instrumentation.runOnMainSync {
            val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            text = clipboard.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.text?.toString()
        }
        return text
    }

    @Test
    fun aSecretIsGoneWhenClearedNow() {
        instrumentation.runOnMainSync { SecretClipboard.copy(context, "synthetic-secret-1", secret = true) }
        assertEquals("synthetic-secret-1", clipboardText())
        instrumentation.runOnMainSync { SecretClipboard.clearNow(context) }
        assertNotEquals("synthetic-secret-1", clipboardText())
    }

    @Test
    fun aSecretIsGoneAfterThirtySeconds() {
        instrumentation.runOnMainSync { SecretClipboard.copy(context, "synthetic-secret-2", secret = true) }
        Thread.sleep(SecretClipboard.CLEAR_AFTER_MS + 2_000)
        assertNotEquals("synthetic-secret-2", clipboardText())
    }

    @Test
    fun whatTheUserCopiedSinceIsLeftAlone() {
        instrumentation.runOnMainSync {
            SecretClipboard.copy(context, "synthetic-secret-3", secret = true)
            SecretClipboard.copy(context, "ordinary text", secret = false)
            SecretClipboard.clearNow(context)
        }
        assertEquals("ordinary text", clipboardText())
    }
}
