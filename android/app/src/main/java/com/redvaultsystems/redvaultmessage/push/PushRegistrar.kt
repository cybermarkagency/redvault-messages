// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.push

import android.app.Activity
import android.content.Context
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.PushApi
import com.redvaultsystems.redvaultmessage.data.api.WebPushEndpointRequest
import com.redvaultsystems.redvaultmessage.data.api.WebPushSubscriptionRequest
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.unifiedpush.android.connector.UnifiedPush

/** How this phone hears about new things while the app is closed, for Settings. */
sealed interface PushMode {
    /** Web Push through the user's UnifiedPush distributor app ([distributor] is its package name). */
    data class UnifiedPush(val distributor: String) : PushMode

    /** No distributor (or the server can't send Web Push): the app's own long poll, [PushFallbackService]. */
    data object BackgroundConnection : PushMode

    data object Off : PushMode
}

enum class PushRoute { UnifiedPush, BackgroundConnection, Off }

private val VAPID_KEY = Regex("^[A-Za-z0-9_-]{87}$")

/** An uncompressed P-256 public key, base64url without padding: what the connector accepts (anything else throws). */
fun isValidVapidKey(key: String?): Boolean = key != null && VAPID_KEY.matches(key)

/**
 * Which way to go after sign-in. Notifications off: nothing at all. Strict no-push means "no push service
 * run by someone else": never UnifiedPush (a distributor talks to its own push server outside Tor), and
 * without Tor routing nothing at all; with Tor routing ([torRouted]) the background connection is still
 * allowed, because it only ever reaches our own onion service through the Tor gateway. Otherwise
 * UnifiedPush when the server has a VAPID key and a distributor is installed, else the background
 * connection if the user allows it (and Android lets the app notify, without which it would run for
 * nothing), else nothing.
 */
fun choosePushRoute(
    enabled: Boolean,
    strictNoPush: Boolean,
    vapidKey: String?,
    hasDistributor: Boolean,
    backgroundConnection: Boolean,
    notificationsAllowed: Boolean,
    torRouted: Boolean = false,
): PushRoute = when {
    !enabled -> PushRoute.Off
    !strictNoPush && isValidVapidKey(vapidKey) && hasDistributor -> PushRoute.UnifiedPush
    else -> fallbackRoute(enabled, strictNoPush, backgroundConnection, notificationsAllowed, torRouted)
}

/** When UnifiedPush can't be used (or stops working, or strict no-push rules it out): the background connection, or nothing. */
fun fallbackRoute(
    enabled: Boolean,
    strictNoPush: Boolean,
    backgroundConnection: Boolean,
    notificationsAllowed: Boolean,
    torRouted: Boolean = false,
): PushRoute =
    if (enabled && (!strictNoPush || torRouted) && backgroundConnection && notificationsAllowed) PushRoute.BackgroundConnection else PushRoute.Off

/**
 * Ties this phone's push route to the signed-in account on the server, and unties it on logout. No Google
 * services: pushes are Web Push through the user's UnifiedPush distributor (ntfy, for example), and
 * without one the app keeps its own connection to our server ([PushFallbackService]). Either way a push
 * says only what kind of thing happened (server/src/services/pushService.ts), so all the server and the
 * push service learn is "this account has a phone that can be woken up".
 *
 * Picking a distributor can need the user (Android's chooser when several are installed), so [register]
 * takes the Activity where sign-in ended up. The distributor answers later, in [UnifiedPushReceiver],
 * which hands the endpoint back here ([onNewEndpoint]).
 */
class PushRegistrar(
    context: Context,
    val api: PushApi,
    private val torBuild: Boolean = false,
    /** True while someone is signed in in this process (AppContainer.hasActiveSession). */
    private val signedIn: () -> Boolean = { true },
) {
    private val appContext = context.applicationContext
    private val prefs = appContext.getSharedPreferences("push", Context.MODE_PRIVATE)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private val _mode = MutableStateFlow<PushMode>(PushMode.Off)
    val mode: StateFlow<PushMode> = _mode.asStateFlow()

    /** The background connection should run but Android wouldn't start it from the background: start it on the next foreground. */
    @Volatile
    private var fallbackPending = false

    /** The user's choice in Settings. On by default. */
    var enabled: Boolean
        get() = prefs.getBoolean("enabled", true)
        set(value) = prefs.edit().putBoolean("enabled", value).apply()

    /**
     * Strict no-push mode (Settings › Network routing): never a push service run by someone else, so no
     * UnifiedPush registration. Without Tor routing that leaves nothing to wake this phone (messages and
     * calls arrive only while the app is open). With Tor routing the background connection still runs if
     * it's switched on: it reaches only our onion service, through the Tor gateway, never directly.
     * Off by default, except with Tor routing: there a UnifiedPush distributor would talk to its own push
     * server outside Tor and learn when messages arrive, so it's on until the user turns it off.
     */
    var strictNoPush: Boolean
        get() = prefs.getBoolean("strictNoPush", torBuild)
        set(value) = prefs.edit().putBoolean("strictNoPush", value).apply()

    /** Settings: "Background connection when no push app is installed". On by default. */
    var backgroundConnection: Boolean
        get() = prefs.getBoolean("backgroundConnection", true)
        set(value) = prefs.edit().putBoolean("backgroundConnection", value).apply()

    /** The UnifiedPush distributor apps installed on this phone (package names). */
    fun distributors(): List<String> = runCatching { UnifiedPush.getDistributors(appContext) }.getOrDefault(emptyList())

    /** A distributor's name as the launcher shows it ("ntfy"), or its package name. */
    fun distributorName(packageName: String): String = runCatching {
        val pm = appContext.packageManager
        pm.getApplicationLabel(pm.getApplicationInfo(packageName, 0)).toString()
    }.getOrDefault(packageName)

    /**
     * After sign-in (and whenever a setting turns notifications back on), from the signed-in screen's
     * [activity]: picks the route and starts it. Best-effort; a failure leaves the background connection
     * or nothing.
     */
    /** True in Tor builds: every request, the background connection's included, goes only through the Tor gateway. */
    val torRouted: Boolean get() = torBuild

    suspend fun register(activity: Activity) {
        if (!enabled || (strictNoPush && !torBuild)) {
            stopFallback()
            _mode.value = PushMode.Off
            return
        }
        val vapid = vapidKey()
        val installed = distributors()
        val hasDistributor = installed.isNotEmpty() || ackDistributor() != null
        when (choosePushRoute(enabled, strictNoPush, vapid, hasDistributor, backgroundConnection, Notifications.allowed(appContext), torBuild)) {
            PushRoute.UnifiedPush -> withContext(Dispatchers.Main) {
                val key = vapid!!
                val remembered = prefs.getString("distributor", null)
                if (remembered != null && remembered in installed && ackDistributor() == null) {
                    // Signed out and back in: the connector forgot the distributor on unregister, we didn't,
                    // so the chooser doesn't come up again for one the user already picked.
                    runCatching { UnifiedPush.saveDistributor(appContext, remembered) }
                    usePush(key)
                } else {
                    runCatching {
                        UnifiedPush.tryUseCurrentOrDefaultDistributor(activity) { ok -> if (ok) usePush(key) else useFallback() }
                    }.onFailure { useFallback() }
                }
            }
            PushRoute.BackgroundConnection -> useFallback()
            PushRoute.Off -> {
                stopFallback()
                _mode.value = PushMode.Off
            }
        }
    }

    /** Settings: "Choose push app" when several distributors are installed. */
    fun pickDistributor(activity: Activity) {
        runCatching {
            UnifiedPush.tryPickDistributor(activity) { ok ->
                if (ok) {
                    scope.launch {
                        val vapid = vapidKey()
                        withContext(Dispatchers.Main) { if (vapid != null && isValidVapidKey(vapid)) usePush(vapid) else useFallback() }
                    }
                }
            }
        }
    }

    /** Settings: the background connection switch. */
    suspend fun setBackgroundConnection(on: Boolean, activity: Activity) {
        backgroundConnection = on
        when {
            !on && _mode.value == PushMode.BackgroundConnection -> {
                stopFallback()
                _mode.value = PushMode.Off
            }
            on && _mode.value == PushMode.Off -> register(activity)
        }
    }

    /**
     * Before logout (the session cookie is still valid): stops the background connection, unregisters from
     * the distributor and removes the endpoint from the server, so nothing more arrives for this account.
     * Also when the user turns notifications off or strict no-push on.
     */
    suspend fun unregister() {
        stopFallback()
        _mode.value = PushMode.Off
        runCatching { UnifiedPush.unregister(appContext) }
        val endpoint = prefs.getString("endpoint", null)
        prefs.edit().remove("endpoint").apply()
        if (endpoint != null) runCatching { apiCall(ApiClient.json) { api.unregisterWebPush(WebPushEndpointRequest(endpoint)) } }
    }

    /** The session ended without a logout (it expired): the background connection can't poll any more. */
    fun sessionEnded() {
        stopFallback()
        if (_mode.value == PushMode.BackgroundConnection) _mode.value = PushMode.Off
    }

    /** The app came to the foreground: start a background connection Android refused to start earlier. */
    fun resumeIfNeeded() {
        if (fallbackPending && !PushFallbackService.running && signedIn()) {
            if (PushFallbackService.start(appContext)) fallbackPending = false
        }
    }

    // --- From UnifiedPushReceiver ---

    /** The distributor gave us an endpoint (new or rotated). [p256dh]/[auth] null: it can't be encrypted to. */
    fun onNewEndpoint(url: String, p256dh: String?, auth: String?) {
        if (!signedIn() || !enabled || strictNoPush) return
        if (p256dh == null || auth == null) {
            useFallback()
            return
        }
        scope.launch {
            val previous = prefs.getString("endpoint", null)
            val uploaded = runCatching { apiCall(ApiClient.json) { api.registerWebPush(WebPushSubscriptionRequest(url, p256dh, auth)) } }.isSuccess
            if (!uploaded) {
                // Not on the server (offline, or it refused the endpoint, e.g. not https): poll instead.
                useFallback()
                return@launch
            }
            prefs.edit().putString("endpoint", url).apply()
            if (previous != null && previous != url) {
                runCatching { apiCall(ApiClient.json) { api.unregisterWebPush(WebPushEndpointRequest(previous)) } }
            }
            val distributor = ackDistributor() ?: runCatching { UnifiedPush.getSavedDistributor(appContext) }.getOrNull()
            if (distributor != null) prefs.edit().putString("distributor", distributor).apply()
            stopFallback()
            _mode.value = PushMode.UnifiedPush(distributor.orEmpty())
        }
    }

    /**
     * The distributor stopped delivering: [unregistered] (it dropped us, or was uninstalled), registration
     * failed, or its server is temporarily down. The background connection takes over while signed in.
     */
    fun onPushLost(unregistered: Boolean) {
        if (unregistered) {
            val endpoint = prefs.getString("endpoint", null)
            prefs.edit().remove("endpoint").apply()
            if (endpoint != null && signedIn()) {
                scope.launch { runCatching { apiCall(ApiClient.json) { api.unregisterWebPush(WebPushEndpointRequest(endpoint)) } } }
            }
        }
        if (signedIn()) useFallback() else _mode.value = PushMode.Off
    }

    /** From [PushFallbackService], when the server no longer takes the session. */
    internal fun fallbackSignedOut() {
        fallbackPending = false
        if (_mode.value == PushMode.BackgroundConnection) _mode.value = PushMode.Off
    }

    // --- Internals ---

    private fun ackDistributor(): String? = runCatching { UnifiedPush.getAckDistributor(appContext) }.getOrNull()

    private fun usePush(vapid: String) {
        val registered = runCatching {
            UnifiedPush.register(appContext, messageForDistributor = "RedVault Messages", vapid = vapid)
        }.isSuccess
        if (!registered) {
            useFallback()
            return
        }
        // Shown at once; the endpoint follows in onNewEndpoint (which stops a running background connection).
        val distributor = runCatching { UnifiedPush.getSavedDistributor(appContext) }.getOrNull()
        _mode.value = PushMode.UnifiedPush(distributor.orEmpty())
    }

    private fun useFallback() {
        val route = fallbackRoute(enabled, strictNoPush, backgroundConnection, Notifications.allowed(appContext), torBuild)
        if (route == PushRoute.Off || !signedIn()) {
            stopFallback()
            _mode.value = PushMode.Off
            return
        }
        _mode.value = PushMode.BackgroundConnection
        // Android 12+ refuses a foreground service started from the background; then on the next foreground.
        fallbackPending = !PushFallbackService.start(appContext)
    }

    private fun stopFallback() {
        fallbackPending = false
        PushFallbackService.stop(appContext)
    }

    /** The server's VAPID key; the last one it gave if it can't be reached right now. */
    private suspend fun vapidKey(): String? = try {
        val key = apiCall(ApiClient.json) { api.config() }.vapidPublicKey?.takeIf(::isValidVapidKey)
        prefs.edit().apply { if (key != null) putString("vapid", key) else remove("vapid") }.apply()
        key
    } catch (e: Exception) {
        if (e is kotlinx.coroutines.CancellationException) throw e
        prefs.getString("vapid", null)?.takeIf(::isValidVapidKey)
    }
}
