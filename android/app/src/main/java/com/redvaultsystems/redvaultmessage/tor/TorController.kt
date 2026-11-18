// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import com.redvaultsystems.tor.NativeTor
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.File

enum class TorState { Connecting, Connected, Unavailable }

data class TorStatus(
    val state: TorState,
    /** Bootstrap progress, 0-100. */
    val progress: Int = 0,
    /** What Tor is doing now, for the status screen ("Loading directory", ...). Never an address. */
    val detail: String = "",
)

/** Where the loopback SOCKS gate listens, and its per-start random credentials. */
class SocksEndpoint(val host: String, val port: Int, val username: String, val password: String) {
    override fun toString(): String = "SocksEndpoint($host:$port, credentials redacted)"
}

/**
 * The embedded Tor gateway (tor-core, Arti) for an onion API base
 * (docs/tor-routing-plan.md). Text, photos, files, Stories and voice messages
 * then leave only through its loopback SOCKS gate, which accepts nothing but
 * the one onion service: there is no direct fallback. Calls are separate
 * (WebRTC through the TURN relay) and never go through here.
 *
 * Mirrors the desktop TorController: requests may go out while Tor is
 * connecting (they wait inside Tor) or connected; "unavailable" stops them,
 * and the apps keep their sealed outbox until it clears.
 */
class TorController(
    /** The onion service's host, e.g. "abc…xyz.onion". */
    val onionHost: String,
    private val dataDir: File,
    /**
     * The sealed backup route's website (docs/tor-sealed-fallback.md), e.g. "www.example.com": the one place outside
     * onion services the gate may reach, on port 443 through a Tor exit. Null: onion services only.
     */
    private val exitHost: String? = null,
    /**
     * The app's native-library folder (ApplicationInfo.nativeLibraryDir), where the bridge programs liblyrebird.so and
     * libsnowflake.so are: the only place Android lets an app run a program it ships. Null: no bridges.
     */
    private val nativeLibDir: File? = null,
) : TorGateSource {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val lock = Any()
    private var handle = 0L
    private var endpoint: SocksEndpoint? = null
    private var pollJob: Job? = null

    private val _status = MutableStateFlow(TorStatus(TorState.Connecting, detail = "Starting Tor"))
    val status: StateFlow<TorStatus> = _status.asStateFlow()

    // --- The sealed backup route (docs/tor-sealed-fallback.md) ---
    private val _backupRoute = MutableStateFlow(false)
    /** True while requests use the sealed backup route. */
    val backupRoute: StateFlow<Boolean> = _backupRoute.asStateFlow()
    private val _onionOnly = MutableStateFlow(false)
    /** The user's choice "Only use the onion address" (off by default; kept on this phone by the app). */
    val onionOnly: StateFlow<Boolean> = _onionOnly.asStateFlow()
    /** Whether this build carries the gateway's key, so a backup route exists at all. */
    @Volatile var backupRouteAvailable: Boolean = false
    /** Called when the user changes "Only use the onion address" (to save it, and leave the backup route). */
    @Volatile var onOnionOnlyChanged: ((Boolean) -> Unit)? = null

    fun reportBackupRoute(backup: Boolean) {
        _backupRoute.value = backup
    }

    /** Sets the choice without calling back (loading the saved value at start). */
    fun loadOnionOnly(onionOnly: Boolean) {
        _onionOnly.value = onionOnly
    }

    fun setOnionOnly(onionOnly: Boolean) {
        if (_onionOnly.value == onionOnly) return
        _onionOnly.value = onionOnly
        onOnionOnlyChanged?.invoke(onionOnly)
    }

    // --- Bridges (tor-core/src/bridges.rs): a hidden way into Tor where it's blocked. Still Tor. ---
    private val _bridgeMode = MutableStateFlow(BRIDGES_OFF)
    /** "off", "snowflake", "obfs4" or "custom" (kept on this phone by the app). */
    val bridgeMode: StateFlow<String> = _bridgeMode.asStateFlow()
    private val _bridgeLines = MutableStateFlow("")
    /** The pasted bridge lines for "custom", one per line. */
    val bridgeLines: StateFlow<String> = _bridgeLines.asStateFlow()
    /** Called when the user saves new bridge settings (to keep them). */
    @Volatile var onBridgesChanged: ((mode: String, lines: String) -> Unit)? = null

    private fun program(name: String): File? = nativeLibDir?.let { File(it, name) }?.takeIf { it.isFile }

    /** Whether this build ships the bridge programs. */
    val bridgesAvailable: Boolean get() = program(LYREBIRD) != null && program(SNOWFLAKE) != null

    /** Sets the saved bridge settings before [start] (no restart, no callback). */
    fun loadBridges(mode: String, lines: String) {
        _bridgeMode.value = if (mode in BRIDGE_MODES) mode else BRIDGES_OFF
        _bridgeLines.value = lines
    }

    private fun bridgesJson(mode: String, lines: String) = buildJsonObject {
        put("mode", mode)
        if (mode == "custom") put("lines", buildJsonArray { lines.lines().forEach { add(it) } })
        program(LYREBIRD)?.let { put("lyrebirdPath", it.absolutePath) }
        program(SNOWFLAKE)?.let { put("snowflakePath", it.absolutePath) }
    }

    /**
     * Checks and saves bridge settings, then restarts Tor with them (what's waiting to send waits a little longer).
     * Returns null when saved, or what's wrong (nothing changes then).
     */
    fun setBridges(mode: String, lines: String): String? {
        if (mode !in BRIDGE_MODES) return "Choose how to connect."
        val trimmed = if (mode == "custom") lines.trim() else ""
        if (mode != BRIDGES_OFF) {
            if (!bridgesAvailable) return "This copy of the app doesn't include bridge support."
            val result = runCatching {
                NativeTor.load()
                json.decodeFromString<ValidationJson>(NativeTor.nativeValidateBridges(bridgesJson(mode, trimmed).toString()))
            }.getOrElse { return "Couldn't check those bridges." }
            if (!result.ok) return result.error ?: "Those bridges can't be used."
        }
        if (mode == _bridgeMode.value && trimmed == _bridgeLines.value) return null
        _bridgeMode.value = mode
        _bridgeLines.value = trimmed
        onBridgesChanged?.invoke(mode, trimmed)
        // Off the caller's (UI) thread: stopping Tor can take a few seconds.
        scope.launch {
            stop()
            start()
        }
        return null
    }

    @Serializable
    private class ValidationJson(val ok: Boolean = false, val count: Int = 0, val error: String? = null)

    private val _connected = MutableSharedFlow<Unit>(extraBufferCapacity = 1)
    /** Each time Tor becomes connected: drain the outbox, resume paused uploads. */
    val becameConnected: SharedFlow<Unit> = _connected.asSharedFlow()

    val connected: Boolean get() = _status.value.state == TorState.Connected

    /** Starts Tor in the background (it takes from a few seconds to a minute). Safe to call again after [stop]. */
    fun start() {
        scope.launch {
            synchronized(lock) {
                if (handle != 0L) return@launch
                val mode = _bridgeMode.value
                _status.value = TorStatus(TorState.Connecting, detail = if (mode == BRIDGES_OFF) "Starting Tor" else "Starting Tor with bridges")
                try {
                    NativeTor.load()
                    // Arti refuses state that other users could read or change: owner-only.
                    val state = File(dataDir, "state")
                    val cache = File(dataDir, "cache")
                    for (dir in listOf(dataDir, state, cache)) ownerOnly(dir)
                    val config = buildJsonObject {
                        put("stateDir", state.absolutePath)
                        put("cacheDir", cache.absolutePath)
                        put("allowedTargets", buildJsonArray { add("$onionHost:80") })
                        exitHost?.let { host -> put("allowedExitTargets", buildJsonArray { add("$host:443") }) }
                        if (mode != BRIDGES_OFF) put("bridges", bridgesJson(mode, _bridgeLines.value))
                    }
                    handle = NativeTor.nativeStart(config.toString())
                    val e = json.decodeFromString<EndpointJson>(NativeTor.nativeEndpoint(handle))
                    check(e.host == "127.0.0.1" || e.host == "::1") { "The Tor gate isn't on loopback" }
                    endpoint = SocksEndpoint(e.host, e.port, e.username, e.password)
                } catch (t: Throwable) {
                    // No gateway (missing library, bad state dir): fail closed, never fall back.
                    handle = 0L
                    endpoint = null
                    _status.value = TorStatus(TorState.Unavailable, detail = "Tor couldn't start")
                    return@launch
                }
            }
            pollJob = scope.launch {
                while (true) {
                    poll()
                    delay(POLL_MS)
                }
            }
        }
    }

    /** Closes the SOCKS port and stops Tor. Everything Tor-routed fails (and queues) until [start]. */
    fun stop() {
        pollJob?.cancel()
        pollJob = null
        synchronized(lock) {
            endpoint = null
            if (handle != 0L) {
                runCatching { NativeTor.nativeStop(handle) }
                handle = 0L
            }
        }
        _status.value = TorStatus(TorState.Unavailable, detail = "Tor is stopped")
    }

    /** Backgrounded with nothing to send: Tor keeps its circuits but stops preemptive work. */
    fun setDormant(dormant: Boolean) {
        synchronized(lock) { if (handle != 0L) runCatching { NativeTor.nativeSetDormant(handle, dormant) } }
    }

    /** The gate to connect through, or null when nothing may be sent (Tor stopped or unavailable). */
    override fun gate(): SocksEndpoint? = synchronized(lock) {
        if (_status.value.state == TorState.Unavailable) null else endpoint
    }

    private fun poll() {
        val raw = synchronized(lock) { if (handle == 0L) return else runCatching { NativeTor.nativeStatus(handle) }.getOrNull() } ?: return
        val parsed = runCatching { json.decodeFromString<StatusJson>(raw) }.getOrNull() ?: return
        val state = when (parsed.state) {
            "connected" -> TorState.Connected
            "connecting" -> TorState.Connecting
            else -> TorState.Unavailable
        }
        val was = _status.value.state
        val next = TorStatus(state, parsed.progress, parsed.detail)
        if (next == _status.value) return
        _status.value = next
        if (was != TorState.Connected && state == TorState.Connected) _connected.tryEmit(Unit)
    }

    @Serializable
    private class EndpointJson(val host: String, val port: Int, val username: String, val password: String)

    @Serializable
    private class StatusJson(val state: String = "unavailable", val progress: Int = 0, val detail: String = "")

    companion object {
        const val BRIDGES_OFF = "off"
        val BRIDGE_MODES = listOf("off", "snowflake", "obfs4", "custom")
        private const val LYREBIRD = "liblyrebird.so"
        private const val SNOWFLAKE = "libsnowflake.so"
        private const val POLL_MS = 1_000L
        private val json = Json { ignoreUnknownKeys = true }

        /** An onion API base routes through Tor; anything else doesn't (a dev server, or Tor routing off). */
        fun isOnionHost(host: String): Boolean = host.lowercase().endsWith(".onion")

        private fun ownerOnly(dir: File) {
            dir.mkdirs()
            dir.setReadable(false, false); dir.setReadable(true, true)
            dir.setWritable(false, false); dir.setWritable(true, true)
            dir.setExecutable(false, false); dir.setExecutable(true, true)
        }
    }
}
