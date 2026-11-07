package com.redvaultsystems.redvaultmessage.ui.components

import android.app.Activity
import android.os.Build
import android.view.Window
import android.view.WindowManager
import androidx.activity.compose.LocalActivity
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.platform.LocalView
import java.util.WeakHashMap

/**
 * Everyone who wants the window kept out of screenshots and Recents, counted, so that one of them
 * letting go can't switch the protection off for the others (leaving the recovery key screen used to
 * clear FLAG_SECURE even with "Hide app content" on). Main thread only.
 */
internal class SecureHolds(private val setSecure: (Boolean) -> Unit) {
    private var holds = 0
    private var applied = false

    fun hold() {
        holds++
        sync()
    }

    fun release() {
        if (holds > 0) holds--
        sync()
    }

    private fun sync() {
        val wanted = holds > 0
        if (wanted != applied) {
            applied = wanted
            setSecure(wanted)
        }
    }
}

private val holdsByWindow = WeakHashMap<Window, SecureHolds>()

private fun holdsFor(activity: Activity): SecureHolds {
    val window = activity.window
    return holdsByWindow.getOrPut(window) {
        SecureHolds { on ->
            if (on) window.addFlags(WindowManager.LayoutParams.FLAG_SECURE)
            else window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE)
        }
    }
}

/**
 * Keeps this window out of screenshots, screen recording, screen sharing and the Recents thumbnail while
 * [active] (FLAG_SECURE). Compose dialogs and menus opened meanwhile inherit it.
 *
 * What it can't do: it stops the system's normal capture paths, not a rooted or compromised device, and
 * not someone pointing a camera at the screen.
 */
@Composable
fun SecureWindow(active: Boolean = true) {
    val activity = LocalActivity.current
    DisposableEffect(activity, active) {
        val holds = activity?.takeIf { active }?.let(::holdsFor)
        holds?.hold()
        onDispose { holds?.release() }
    }
}

/** How this Android version can keep a window's contents out of the Recents thumbnail. */
internal enum class RecentsProtection {
    /** Android 13+: no thumbnail of this window; screenshots still work. */
    NoThumbnail,

    /** Older versions have only FLAG_SECURE, which blocks the person's own screenshots as well. */
    SecureWindow,
    None,
    ;

    companion object {
        fun forDevice(sdk: Int, hide: Boolean): RecentsProtection = when {
            !hide -> None
            sdk >= 33 -> NoThumbnail
            else -> SecureWindow
        }
    }
}

/** Keeps what's on screen out of the Recents thumbnail while [hide], by the means this Android version has. */
@Composable
fun HideFromRecents(hide: Boolean) {
    val protection = RecentsProtection.forDevice(Build.VERSION.SDK_INT, hide)
    SecureWindow(protection == RecentsProtection.SecureWindow)
    val activity = LocalActivity.current
    DisposableEffect(activity, protection) {
        if (protection == RecentsProtection.NoThumbnail && Build.VERSION.SDK_INT >= 33) activity?.setRecentsScreenshotEnabled(false)
        onDispose {
            if (protection == RecentsProtection.NoThumbnail && Build.VERSION.SDK_INT >= 33) activity?.setRecentsScreenshotEnabled(true)
        }
    }
}

/**
 * While [active], touches that reach this window through another app's window on top of it are dropped
 * (View.filterTouchesWhenObscured): a tapjacking overlay can't get a tap on what it hides.
 *
 * It's set on the root view of whichever window this composable is in: AndroidComposeView handles touches
 * itself and never reaches the View code that does the filtering, so it has to happen above it. A dialog
 * is a window of its own, so a dialog that confirms something sensitive needs this inside it too.
 *
 * What it can't do: it only covers a touch at a spot another window covers. A window that leaves the
 * button itself uncovered isn't caught (only FLAG_WINDOW_IS_PARTIALLY_OBSCURED says so), and Android 12+
 * already refuses most such overlays by itself.
 */
@Composable
fun TouchGuard(active: Boolean = true) {
    val view = LocalView.current
    DisposableEffect(view, active) {
        val root = view.rootView
        val before = root.filterTouchesWhenObscured
        if (active) root.filterTouchesWhenObscured = true
        onDispose { root.filterTouchesWhenObscured = before }
    }
}
