package com.redvaultsystems.redvaultmessage.ui.lock

import android.os.SystemClock
import androidx.compose.runtime.staticCompositionLocalOf
import com.redvaultsystems.redvaultmessage.data.AppLockStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext

sealed interface UnlockResult {
    data object Unlocked : UnlockResult
    data class Wrong(val attemptsLeft: Int) : UnlockResult
    /** Too many wrong PINs: the caller ends the session. */
    data object LockedOut : UnlockResult
}

/**
 * Locks the signed-in app after it's been in the background longer than the
 * chosen timeout. Driven by the whole process going to the background (not
 * one screen), so rotating the phone never locks it.
 */
class AppLock(val store: AppLockStore) {
    private val _locked = MutableStateFlow(false)
    val locked: StateFlow<Boolean> = _locked.asStateFlow()
    private val _enabled = MutableStateFlow(store.enabled)
    /** A PIN is set, so what's on screen stays out of the Recents thumbnail. */
    val enabled: StateFlow<Boolean> = _enabled.asStateFlow()
    private var backgroundedAt: Long? = null
    private var externalStartedAt: Long? = null

    /** Call after setting or removing the PIN. */
    fun pinChanged() {
        _enabled.value = store.enabled
    }

    /**
     * Call just before opening a system screen for the app itself (file or
     * photo picker, "save as"): coming back from it doesn't lock, so the
     * picked file isn't lost behind the lock screen. Only once, and only if
     * the user comes back within [EXTERNAL_GRACE_MS].
     */
    fun expectExternalReturn() {
        externalStartedAt = SystemClock.elapsedRealtime()
    }

    fun onBackground() {
        backgroundedAt = SystemClock.elapsedRealtime()
    }

    fun onForeground() {
        val since = backgroundedAt ?: return
        backgroundedAt = null
        val now = SystemClock.elapsedRealtime()
        val external = externalStartedAt
        externalStartedAt = null
        if (external != null && now - external < EXTERNAL_GRACE_MS) return
        if (store.enabled && now - since >= store.timeout.millis) _locked.value = true
    }

    fun lockNow() {
        if (store.enabled) _locked.value = true
    }

    suspend fun unlock(pin: String): UnlockResult {
        val ok = withContext(Dispatchers.Default) { store.verify(pin) }
        return when {
            ok -> {
                _locked.value = false
                UnlockResult.Unlocked
            }
            store.failedAttempts >= AppLockStore.MAX_ATTEMPTS -> {
                store.clear()
                pinChanged()
                UnlockResult.LockedOut
            }
            else -> UnlockResult.Wrong(AppLockStore.MAX_ATTEMPTS - store.failedAttempts)
        }
    }

    /** "Forgot PIN": the PIN is removed and the session ends; the password gets them back in. */
    fun forget() {
        store.clear()
        pinChanged()
        _locked.value = false
    }

    companion object {
        const val EXTERNAL_GRACE_MS = 10 * 60 * 1000L
    }
}

/** The signed-in app's lock, for screens that open a system picker. Null outside a session. */
val LocalAppLock = staticCompositionLocalOf<AppLock?> { null }
