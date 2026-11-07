package com.redvaultsystems.redvaultmessage.ui.settings

import android.content.Context
import com.goterl.lazysodium.LazySodium
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.Commitment
import com.redvaultsystems.redvaultmessage.crypto.DriveCrypto
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.Merkle
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.CallApi
import com.redvaultsystems.redvaultmessage.data.api.CheckpointDto
import com.redvaultsystems.redvaultmessage.data.api.IceServerDto
import com.redvaultsystems.redvaultmessage.data.api.LogEntryDto
import com.redvaultsystems.redvaultmessage.data.api.SealedLabelDto
import com.redvaultsystems.redvaultmessage.data.api.SecurityApi
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.net.URI

data class LinkedDevice(val id: String, val current: Boolean, val awaitingTwoFactor: Boolean, val expiresOn: String, val name: String)

data class DevicesState(val loaded: Boolean = false, val devices: List<LinkedDevice> = emptyList(), val error: String? = null)

enum class Tone { Muted, Ok, Warn }

data class RelayRow(val url: String, val host: String, val transport: String, val status: String = "Not checked", val tone: Tone = Tone.Muted)

data class RelaysState(
    val apiHost: String,
    val apiStatus: String = "Not checked",
    val apiTone: Tone = Tone.Muted,
    val relays: List<RelayRow> = emptyList(),
    val error: String? = null,
)

/** The latest checkpoint, as checked on this phone. */
data class CheckedCheckpoint(
    val treeSize: Long,
    val rootHash: String,
    val signedAt: String,
    val keyFingerprint: String,
    /** "pinned", "first-use" or "changed". */
    val keyStatus: String,
    /** Consistent with the last checkpoint this phone verified; null when there was none to compare with. */
    val consistent: Boolean?,
)

/** One listed entry: "checking", "verified", "failed" or (for your own) "mismatch". */
data class LogRow(val entry: LogEntryDto, val mine: Boolean, val status: String)

data class LogState(
    val loaded: Boolean = false,
    /** This server has no transparency log. */
    val disabled: Boolean = false,
    val error: String? = null,
    val checkpoint: CheckedCheckpoint? = null,
    val rows: List<LogRow> = emptyList(),
    val search: String = "",
    val onlyMine: Boolean = false,
    val checkingMine: Boolean = false,
    val mineMatched: Int = 0,
    val mineMismatches: Int = 0,
    val minePending: Int = 0,
)

/**
 * Settings' security pages, ports of the desktop's LinkedDevices,
 * RelayCheck and TransparencyLog:
 *   - Linked devices: everywhere this account is signed in. The server
 *     keeps only an id and the expiry day; names are encrypted by the
 *     account's own devices with a key derived from the private key.
 *   - Relay servers: the message relay and the TURN relays, with a check
 *     that only asks each relay for an address (no media, no identity).
 *   - Transparency log: the checkpoint's signature under a key pinned on
 *     first use, an append-only (consistency) proof against the last
 *     checkpoint this phone verified, and inclusion proofs for every listed
 *     entry and for your own recent messages, all checked here.
 */
class SecuritySession(
    context: Context,
    private val security: SecurityApi,
    private val api: ApiService,
    private val calls: CallApi,
    private val sodium: LazySodium,
    private val drive: DriveCrypto,
    private val e2e: E2eCrypto,
    private val identity: SelfIdentity,
    apiBase: String,
    /** Asks a TURN relay for an address; returns the milliseconds it took, or null if it couldn't. */
    private val probeRelay: suspend (IceServerDto, String) -> Long?,
    private val conversations: () -> List<Conversation>,
    private val onSessionExpired: () -> Unit,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val apiHost = runCatching { URI(apiBase).host }.getOrNull().orEmpty()
    private val prefs = context.getSharedPreferences("transparencyLog", Context.MODE_PRIVATE)

    private val _devices = MutableStateFlow(DevicesState())
    val devices: StateFlow<DevicesState> = _devices.asStateFlow()
    private val _relays = MutableStateFlow(RelaysState(apiHost))
    val relays: StateFlow<RelaysState> = _relays.asStateFlow()
    private val _log = MutableStateFlow(LogState())
    val log: StateFlow<LogState> = _log.asStateFlow()

    // --- Linked devices ---

    private var namedOnce = false
    private val labelKey: ByteArray get() = drive.deriveLocalKey(identity.privateKey, "redvault-device-label-v1")

    fun loadDevices() {
        scope.launch {
            try {
                val key = labelKey
                val list = apiCall(ApiClient.json) { security.sessions() }.map { s ->
                    val name = s.label?.let { label ->
                        runCatching { e2e.decryptWithConversationKey(Base64Codec.decode(label.ciphertext), Base64Codec.decode(label.nonce), key) }.getOrNull()
                    }.orEmpty()
                    LinkedDevice(s.id, s.current, s.awaitingTwoFactor, s.expiresOn.take(10), name)
                }.sortedByDescending { it.current } // this device first
                _devices.value = DevicesState(loaded = true, devices = list)
                // Name this device the first time it's seen here (once: a failed save isn't retried in a loop).
                if (!namedOnce && list.any { it.current && it.name.isEmpty() }) {
                    namedOnce = true
                    saveLabel(defaultDeviceName(), reportFailure = false)
                }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                if (e is ApiException && e.status == 401) onSessionExpired()
                _devices.update { it.copy(loaded = true, error = "Couldn't load your devices. Try again.") }
            }
        }
    }

    fun rename(name: String) {
        if (name.isNotBlank()) saveLabel(name, reportFailure = true)
    }

    private fun saveLabel(name: String, reportFailure: Boolean) {
        scope.launch {
            try {
                val sealed = e2e.encryptWithConversationKey(name.trim().take(60), labelKey)
                apiCall(ApiClient.json) { security.setLabel(SealedLabelDto(Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce))) }
                loadDevices()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                if (reportFailure) _devices.update { it.copy(error = "Couldn't rename this device.") }
            }
        }
    }

    fun signOut(sessionId: String) = deviceAction("Couldn't sign that device out. Try again.") { security.signOut(sessionId) }

    fun signOutOthers() = deviceAction("Couldn't sign the other devices out. Try again.") { security.signOutOthers() }

    private fun deviceAction(failure: String, block: suspend () -> Unit) {
        scope.launch {
            try {
                apiCall(ApiClient.json) { block() }
                loadDevices()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                _devices.update { it.copy(error = failure) }
            }
        }
    }

    /** Like the desktop's "Desktop app · Windows": the kind of app only, never the phone's model or any identifier. */
    private fun defaultDeviceName(): String = "Android app"

    // --- Relay servers ---

    private var iceServer: IceServerDto? = null

    fun loadRelays() {
        scope.launch {
            try {
                val server = apiCall(ApiClient.json) { calls.relays() }.iceServers.firstOrNull()
                iceServer = server
                _relays.update { it.copy(error = null, relays = server?.urls.orEmpty().map { url -> RelayRow(url, hostOf(url), transportOf(url)) }) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                _relays.update {
                    it.copy(error = if (e is ApiException && e.code == "calls_disabled") "No call relay is set up on this server, so calls are off." else "Couldn't load the call relays.")
                }
            }
        }
    }

    fun checkRelays() {
        _relays.update { it.copy(apiStatus = "Checking…", apiTone = Tone.Muted) }
        scope.launch {
            val started = System.currentTimeMillis()
            val ok = runCatching { apiCall(ApiClient.json) { api.me() } }.isSuccess
            val ms = System.currentTimeMillis() - started
            _relays.update { if (ok) it.copy(apiStatus = "Reachable · $ms ms", apiTone = Tone.Ok) else it.copy(apiStatus = "Unreachable from here", apiTone = Tone.Warn) }
        }
        val server = iceServer ?: return
        for (row in _relays.value.relays) {
            setRelay(row.url, "Checking…", Tone.Muted)
            // Times how long it takes to get a relay address. Sends no media and no identity.
            scope.launch {
                val ms = runCatching { probeRelay(server, row.url) }.getOrNull()
                if (ms != null) setRelay(row.url, "Reachable · $ms ms", Tone.Ok) else setRelay(row.url, "Unreachable from here", Tone.Warn)
            }
        }
    }

    private fun setRelay(url: String, status: String, tone: Tone) =
        _relays.update { s -> s.copy(relays = s.relays.map { if (it.url == url) it.copy(status = status, tone = tone) else it }) }

    private fun hostOf(url: String) = url.removePrefix("turns:").removePrefix("turn:").substringBefore('?')

    private fun transportOf(url: String) = when {
        url.startsWith("turns:") -> "TLS over TCP"
        "transport=" in url -> url.substringAfter("transport=").substringBefore('&').uppercase()
        else -> "UDP"
    }

    // --- Transparency log ---

    private var checkpoint: CheckpointDto? = null
    private var entries: List<LogEntryDto> = emptyList()
    private var mineRows: List<LogEntryDto> = emptyList()
    /** Inclusion results for listed entries, per checkpoint ("index@size:root"). */
    private val proofs = HashMap<String, Boolean>()

    private class Mine(val messageId: String, val logIndex: Long?, val status: String)

    private var mine: List<Mine> = emptyList()
    private var mineCheckedAt = -1L
    private var liveJob: Job? = null
    private var verifying = false

    private fun pref(name: String) = "$apiHost/$name"

    /** Starts checking (and re-checking every 15 s while the log page is open). */
    fun openLog() {
        liveJob?.cancel()
        liveJob = scope.launch {
            while (true) {
                refreshLog()
                delay(LIVE_POLL_MS)
                if (_log.value.disabled) break
            }
        }
    }

    fun closeLog() {
        liveJob?.cancel()
        liveJob = null
    }

    fun setSearch(text: String) {
        _log.update { it.copy(search = text) }
        scope.launch { refreshLog() }
    }

    fun setOnlyMine(on: Boolean) {
        _log.update { it.copy(onlyMine = on) }
        scope.launch {
            if (on) loadMineRows()
            publishRows()
            verifyRows()
        }
    }

    private suspend fun refreshLog() {
        try {
            val cp = apiCall(ApiClient.json) { security.checkpoint() }.checkpoint
            var checked: CheckedCheckpoint? = null
            if (cp != null) {
                val valid = withContext(Dispatchers.Default) { Merkle.signatureValid(sodium, cp.treeSize, cp.rootHash, cp.signedAt, cp.signature, cp.publicKey) }
                if (!valid) throw LogCheckFailed("The latest checkpoint's signature doesn't check out.")
                val pinned = prefs.getString(pref("publicKey"), null)
                val keyStatus = when {
                    pinned == null -> "first-use"
                    pinned == cp.publicKey -> "pinned"
                    else -> "changed"
                }
                val lastSize = prefs.getLong(pref("lastTreeSize"), -1)
                val lastRoot = prefs.getString(pref("lastRootHash"), null)
                var consistent: Boolean? = null
                if (keyStatus != "changed" && lastSize >= 0 && lastRoot != null) {
                    consistent = when {
                        lastSize == cp.treeSize -> lastRoot == cp.rootHash
                        lastSize > cp.treeSize -> false // the log shrank: rolled back or forked
                        else -> {
                            val path = apiCall(ApiClient.json) { security.consistency(lastSize, cp.treeSize) }.path
                            withContext(Dispatchers.Default) {
                                Merkle.verifyConsistency(lastSize, cp.treeSize, Merkle.fromHex(lastRoot), Merkle.fromHex(cp.rootHash), path.map(Merkle::fromHex))
                            }
                        }
                    }
                }
                // Remembered only once the checks pass: the next visit compares against this.
                val edit = prefs.edit()
                if (keyStatus == "first-use") edit.putString(pref("publicKey"), cp.publicKey)
                if (keyStatus != "changed" && consistent != false) edit.putLong(pref("lastTreeSize"), cp.treeSize).putString(pref("lastRootHash"), cp.rootHash)
                edit.apply()
                checked = CheckedCheckpoint(cp.treeSize, cp.rootHash, cp.signedAt, Merkle.keyFingerprint(cp.publicKey), keyStatus, consistent)
            }
            val newCheckpoint = checkpoint?.treeSize != cp?.treeSize || checkpoint?.rootHash != cp?.rootHash
            checkpoint = cp
            val q = _log.value.search.trim().ifEmpty { null }
            entries = apiCall(ApiClient.json) { security.entries(50, q) }
            _log.update { it.copy(loaded = true, disabled = false, error = null, checkpoint = checked) }
            publishRows()
            verifyRows()
            if (cp != null && (newCheckpoint || mineCheckedAt != cp.treeSize)) checkMine(cp)
        } catch (e: CancellationException) {
            throw e
        } catch (e: LogCheckFailed) {
            _log.update { it.copy(loaded = true, error = "The log didn't pass the checks on this phone. Treat it as untrusted. (${e.message})") }
        } catch (e: ApiException) {
            if (e.code == "log_disabled" || e.status == 404) _log.update { it.copy(loaded = true, disabled = true) }
            else _log.update { it.copy(loaded = true, error = "Couldn't reach the log. Try again.") }
        } catch (e: Exception) {
            _log.update { it.copy(loaded = true, error = "Couldn't reach the log. Try again.") }
        }
    }

    /** Trust the log's new signing key (after checking it with the operator). */
    fun acceptNewKey() {
        val cp = checkpoint ?: return
        prefs.edit().putString(pref("publicKey"), cp.publicKey).remove(pref("lastTreeSize")).remove(pref("lastRootHash")).apply()
        scope.launch { refreshLog() }
    }

    private fun proofKey(index: Long, cp: CheckpointDto) = "$index@${cp.treeSize}:${cp.rootHash}"

    private fun shownEntries(): List<LogEntryDto> = if (_log.value.onlyMine) mineRows else entries

    private suspend fun inclusionOk(commitmentHex: String, index: Long, cp: CheckpointDto): Boolean {
        if (index >= cp.treeSize) return false
        val proof = apiCall(ApiClient.json) { security.proof(index, cp.treeSize) }
        return withContext(Dispatchers.Default) {
            val leaf = Merkle.leafHash(Merkle.fromHex(commitmentHex))
            Merkle.toHex(leaf) == proof.leafHash.lowercase() &&
                Merkle.verifyInclusion(leaf, index, cp.treeSize, proof.path.map(Merkle::fromHex), Merkle.fromHex(cp.rootHash))
        }
    }

    /** Each listed row is checked here, not taken on the server's word. */
    private suspend fun verifyRows() {
        val cp = checkpoint ?: return
        if (verifying) return
        verifying = true
        try {
            for (e in shownEntries()) {
                val key = proofKey(e.index, cp)
                if (proofs.containsKey(key)) continue
                proofs[key] = runCatching { inclusionOk(e.commitment, e.index, cp) }.getOrDefault(false)
                publishRows()
            }
        } finally {
            verifying = false
        }
    }

    /** Your own recent messages: commitment computed here, entry number from the server, proof checked here. */
    private suspend fun checkMine(cp: CheckpointDto) {
        _log.update { it.copy(checkingMine = true) }
        val own = conversations().flatMap { c -> c.messages.filter { it.fromMe && it.ciphertextB64 != null && it.nonceB64 != null } }
            .sortedByDescending { it.createdAtMs }.take(MAX_OWN_CHECKS)
        val results = mutableListOf<Mine>()
        for (m in own) {
            try {
                val local = Commitment.computeHex(m.id, Base64Codec.decode(m.ciphertextB64!!), Base64Codec.decode(m.nonceB64!!))
                val v = apiCall(ApiClient.json) { api.getMessageVerification(m.id) }
                val status = when {
                    !Commitment.matches(local, v.commitment) -> "mismatch"
                    v.logIndex == null || v.logIndex >= cp.treeSize -> "pending"
                    inclusionOk(local, v.logIndex, cp) -> "match"
                    else -> "mismatch"
                }
                results += Mine(m.id, v.logIndex, status)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                // From before the log existed, or a transient error: not counted either way.
            }
        }
        mine = results
        mineCheckedAt = cp.treeSize
        _log.update {
            it.copy(
                checkingMine = false,
                mineMatched = results.count { r -> r.status == "match" },
                mineMismatches = results.count { r -> r.status == "mismatch" },
                minePending = results.count { r -> r.status == "pending" },
            )
        }
        if (_log.value.onlyMine) loadMineRows()
        publishRows()
    }

    private suspend fun loadMineRows() {
        val rows = mutableListOf<LogEntryDto>()
        for (index in mine.mapNotNull { it.logIndex }.take(25)) {
            runCatching { apiCall(ApiClient.json) { security.entries(1, "#$index") } }.getOrNull()?.let { rows += it }
        }
        mineRows = rows.sortedByDescending { it.index }
    }

    private fun publishRows() {
        val cp = checkpoint
        val mineByIndex = mine.filter { it.logIndex != null }.associateBy { it.logIndex!! }
        val rows = shownEntries().map { e ->
            val own = mineByIndex[e.index]
            val proof = cp?.let { proofs[proofKey(e.index, it)] }
            val status = when {
                own?.status == "mismatch" -> "mismatch"
                proof == true -> "verified"
                proof == false -> "failed"
                else -> "checking"
            }
            LogRow(e, own != null, status)
        }
        _log.update { it.copy(rows = rows) }
    }

    /** The inclusion proof path for one entry, checked again on this phone. */
    suspend fun recheck(entry: LogEntryDto): Pair<Boolean, List<String>> {
        val cp = checkpoint ?: return false to emptyList()
        return try {
            val proof = apiCall(ApiClient.json) { security.proof(entry.index, cp.treeSize) }
            val ok = inclusionOk(entry.commitment, entry.index, cp)
            proofs[proofKey(entry.index, cp)] = ok
            publishRows()
            ok to proof.path
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            false to emptyList()
        }
    }

    fun close() = scope.cancel()

    private class LogCheckFailed(message: String) : Exception(message)

    companion object {
        const val LIVE_POLL_MS = 15_000L
        const val MAX_OWN_CHECKS = 40
    }
}
