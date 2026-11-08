// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
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
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/** A device in the account's signed list. [signal]: Signal sessions are on for this device, declared in the signed list. */
data class TrustedDevice(val deviceId: String, val fingerprint: String, val thisPhone: Boolean, val signal: Boolean = false)

/** A device registered with the server but not in the signed list. [asking] means it has a pairing request open. */
data class WaitingDevice(val deviceId: String, val asking: Boolean)

data class DeviceKeysState(
    /** null while checking; false when this build has no engine, or the server has protocol 3 off. */
    val available: Boolean? = null,
    val busy: Boolean = false,
    val error: String? = null,
    val hasDevice: Boolean = false,
    /** This phone's device holds the account's list (false while it waits to be linked). */
    val linked: Boolean = false,
    val listExists: Boolean = false,
    val trusted: Boolean = false,
    val listedHere: Boolean = true,
    val devices: List<TrustedDevice> = emptyList(),
    val waiting: List<WaitingDevice> = emptyList(),
    val pendingReset: PendingReset? = null,
    val events: List<V3Event> = emptyList(),
    /** "hardware", "software", "unknown" or "none": what protects the key that wraps this phone's device keys. */
    val protection: String = "none",
    /** Shown once, never stored: the recovery key just made. */
    val recoveryKey: String? = null,
    /** This phone is pairing as the new device: waiting for a trusted device (codes null) or comparing codes. */
    val joining: Boolean = false,
    val joiningCodes: List<Int>? = null,
    /** This phone is linking by QR code: the text of the code it shows for a trusted phone to scan. */
    val qrOffer: String? = null,
    /** A trusted phone just linked a scanned device. */
    val qrLinked: Boolean = false,
    /** This phone is answering [answeringDeviceId]'s request and shows the codes to compare. */
    val answeringDeviceId: String? = null,
    val answeringCodes: List<Int>? = null,
    /** What only this phone knows happened to its keys (an older copy of them was refused), oldest first. */
    val localEvents: List<LocalDeviceEvent> = emptyList(),
)

/** What the automatic "ensure device keys" check decided (docs/signal-integration/07-step1-default-path.md section 1). */
enum class EnsureStep {
    /** Nothing to do, or nothing can be known now (offline, the server's answer failed): tried again at the next trigger. */
    Nothing,
    /** The account has no signed device list: this phone becomes its first device. */
    SetUpFirstDevice,
    /** This phone is a trusted device: Signal declared in the list, prekeys topped up. */
    MaintainSignal,
    /** The account has a list and this phone isn't in it: never automatic, the person links it (the chats banner). */
    NeedsLink,
}

/**
 * The rule of the "ensure device keys" check, apart from the work. [enabled] and [listExists] are null when the
 * question failed: a failed check is never taken for "off" or "no list". [linkedHere]: this phone holds a device of
 * the account already; it's never replaced by a new first device on the server's word alone.
 */
fun ensureStep(hasEngine: Boolean, enabled: Boolean?, listExists: Boolean?, linkedHere: Boolean, trustedHere: Boolean): EnsureStep = when {
    !hasEngine || enabled != true || listExists == null -> EnsureStep.Nothing
    !listExists -> if (linkedHere) EnsureStep.Nothing else EnsureStep.SetUpFirstDevice
    trustedHere -> EnsureStep.MaintainSignal
    else -> EnsureStep.NeedsLink
}

/** What the chats screen shows about device keys. */
data class DeviceKeysNotice(
    /** The account has device keys and this phone isn't linked to them: a banner that opens Devices & encryption. */
    val needsLink: Boolean = false,
    /** Shown once after this phone became the first device: create a recovery key. */
    val recoveryKey: Boolean = false,
)

/**
 * Settings > Devices & encryption: protocol 3, per-device identities, with Signal sessions between devices that
 * declare them (docs/multi-device-threat-model.md, docs/signal-integration/). Used when the server has
 * E2E_V3_ENABLED. Set up without asking on the first device ([ensure]); a further device is always linked by the
 * person. A port of ui/src/screens/settings/DeviceKeys.tsx.
 */
class DeviceKeysSession(
    private val userId: String,
    /** The account's current (protocol 1) private key: carried into the first device's certificate, never sent. */
    private val legacyPrivateKey: () -> ByteArray,
    private val storage: DeviceStorage,
    private val engine: EngineRunner?,
    private val api: V3Api,
    private val onSessionExpired: () -> Unit = {},
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(DeviceKeysState())
    val state: StateFlow<DeviceKeysState> = _state.asStateFlow()

    private val _notice = MutableStateFlow(DeviceKeysNotice(recoveryKey = storage.flag(userId, RECOVERY_NOTICE)))
    val notice: StateFlow<DeviceKeysNotice> = _notice.asStateFlow()

    // One "ensure" at a time, and never at the same time as the screen's own set-up.
    private val ensureLock = Mutex()
    private var ensureJob: Job? = null

    private var joinJob: Job? = null
    private var answerJob: Job? = null
    private var newPairing: NewDevicePairing? = null
    private var trustedPairing: TrustedPairing? = null

    fun close() {
        scope.cancel()
    }

    private fun message(e: Throwable): String = when (e) {
        is EngineException -> e.message ?: "The encryption engine refused."
        is ApiException -> e.message ?: "The server refused."
        else -> "Something went wrong. Try again."
    }

    private suspend fun <T> blocking(block: suspend () -> T): T = withContext(Dispatchers.Default) { block() }

    // One instance per device: the chat loops and the screen must not each hold their own copy of its state.
    @Volatile private var cachedDevice: V3Device? = null
    @Volatile private var enabledAt = 0L
    @Volatile private var enabledValue = false
    @Volatile private var enabledKnown = false

    private fun device(): V3Device? = cachedDevice ?: engine?.let { V3Device.open(userId, storage, it, api) }?.also { cachedDevice = it }

    /** Forget the open instance after the device on disk changed (set up, restored, forgotten, linked). */
    private fun dropDevice() {
        cachedDevice = null
    }

    /**
     * The device the chat path uses: null unless this build has the engine, the server has protocol 3 on
     * (asked at most once a minute), and this phone is a trusted device in the account's list.
     */
    suspend fun chatDevice(): V3Device? = blocking {
        if (!serverEnabled()) return@blocking null
        val device = device() ?: return@blocking null
        val own = device.op("deviceList")
        if (own.ok && own.bool("trusted") == true) device else null
    }

    /** Whether this build has the engine and the server has protocol 3 on (asked at most once a minute). */
    private suspend fun serverEnabled(): Boolean {
        if (engine == null) return false
        val now = System.currentTimeMillis()
        if (!enabledKnown || now - enabledAt > 60_000) {
            // Only a real answer from the server is remembered: a failed check (offline, the session not ready yet)
            // must never be taken for "protocol 3 is off", or a chat would quietly fall back to the usual encryption.
            try {
                enabledValue = api.enabled()
                enabledKnown = true
                enabledAt = now
            } catch (e: kotlinx.coroutines.CancellationException) {
                throw e
            } catch (e: Exception) {
                enabledKnown = false
                return false
            }
        }
        return enabledValue
    }

    /**
     * This account uses device keys (it has a signed list on the server) but this phone isn't one of its trusted
     * devices: it never set up, was cleared or reinstalled, or was removed. Without this check such a phone would
     * quietly send the usual way to chats the account's other devices use device keys for.
     */
    suspend fun accountUsesKeysWithoutThisPhone(): Boolean = blocking {
        if (!serverEnabled()) return@blocking false
        val device = device()
        val trusted = device != null && device.op("deviceList").let { it.ok && it.bool("trusted") == true }
        if (trusted) return@blocking false
        runCatching { api.deviceList(userId).deviceList != null }.getOrDefault(false)
    }

    // --- Ensure device keys: without asking, after sign-in, on start and unlock, and every 6 hours ---

    /** Runs [ensure] now and then every 6 hours while signed in. Failures are silent; the next run tries again. */
    fun startEnsuring() {
        if (ensureJob != null) return
        ensureJob = scope.launch {
            while (true) {
                ensureQuietly()
                delay(ENSURE_EVERY_MS)
            }
        }
    }

    /** One more [ensure] soon (the app came back or was unlocked). Skipped while one is running. */
    fun ensureSoon() {
        scope.launch { ensureQuietly() }
    }

    private suspend fun ensureQuietly() {
        if (!ensureLock.tryLock()) return
        try {
            ensureLocked()
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // Silent: retried at the next trigger. Logged nowhere on purpose, the message could name a key.
        } finally {
            ensureLock.unlock()
        }
    }

    /**
     * The "ensure device keys" check (07-step1 section 1): no signed list → this phone becomes the first device and
     * turns Signal on; trusted → Signal kept declared and topped up; a list without this phone → [notice] asks the
     * person to link it. Throws what the server or the engine refused; the caller retries later.
     */
    suspend fun ensure(): EnsureStep = ensureLock.withLock { ensureLocked() }

    private suspend fun ensureLocked(): EnsureStep = blocking {
        val enabled: Boolean? = if (engine == null) {
            false
        } else {
            try {
                api.enabled()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                null
            }
        }
        if (enabled == false) _notice.update { it.copy(needsLink = false) }
        if (enabled != true) return@blocking EnsureStep.Nothing
        val list = api.deviceList(userId).deviceList
        val device = device()
        var linked = false
        var trusted = false
        if (device != null) {
            linked = device.op("deviceList").ok
            if (linked && list != null) {
                // The server's list first: a phone removed by another device mustn't go on as trusted.
                val checked = device.op("verifyDeviceList", "deviceList" to list)
                if (!checked.ok) return@blocking EnsureStep.Nothing
                val own = device.op("deviceList")
                trusted = checked.bool("thisDeviceListed") != false && own.ok && own.bool("trusted") == true
            }
        }
        val step = ensureStep(engine != null, enabled, list != null, linked, trusted)
        when (step) {
            EnsureStep.SetUpFirstDevice -> {
                dropDevice()
                val key = legacyPrivateKey()
                val made = try {
                    setUpFirstDevice(userId, key, storage, engineRunner(), api)
                } finally {
                    key.fill(0)
                }
                dropDevice()
                storage.setFlag(userId, RECOVERY_NOTICE, true)
                _notice.update { it.copy(needsLink = false, recoveryKey = true) }
                keepSignal(made)
            }
            EnsureStep.MaintainSignal -> {
                _notice.update { it.copy(needsLink = false) }
                keepSignal(device!!)
            }
            EnsureStep.NeedsLink -> _notice.update { it.copy(needsLink = true) }
            EnsureStep.Nothing -> {}
        }
        step
    }

    /** Signal on, declared in the signed list, keys topped up. A failure here waits for the next run. */
    private suspend fun keepSignal(device: V3Device) {
        if (!com.redvaultsystems.redvaultmessage.BuildConfig.E2E_SIGNAL) return
        try {
            maintainSignal(device)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // Logged nowhere on purpose: the message could name a key.
        }
    }

    /** The person saw the recovery-key notice (opened it or said not now): it isn't shown again. */
    fun dismissRecoveryNotice() {
        storage.setFlag(userId, RECOVERY_NOTICE, false)
        _notice.update { it.copy(recoveryKey = false) }
    }

    /** The person read the notice that an older copy of this phone's keys was refused. */
    fun clearLocalEvents() {
        storage.clearLocalEvents(userId)
        _state.update { it.copy(localEvents = emptyList()) }
    }

    /** Checks the build and the server, then loads everything the screen shows. */
    fun refresh() {
        scope.launch {
            if (engine == null) {
                _state.update { it.copy(available = false) }
                return@launch
            }
            // A failed check isn't "off": the screen says it couldn't ask, and keeps what it had.
            val available = try {
                blocking { api.enabled() }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                _state.update { it.copy(error = "Couldn't reach the server. Try again.") }
                return@launch
            }
            _state.update { it.copy(available = available, error = null) }
            if (available) load()
        }
    }

    private suspend fun load() {
        try {
            val loaded = blocking { loadState() }
            _notice.update { it.copy(needsLink = loaded.listExists && !loaded.trusted) }
            _state.update { loaded.copy(busy = it.busy, joining = it.joining, joiningCodes = it.joiningCodes, answeringDeviceId = it.answeringDeviceId, answeringCodes = it.answeringCodes, recoveryKey = it.recoveryKey, available = it.available, qrOffer = it.qrOffer, qrLinked = it.qrLinked) }
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
            _state.update { it.copy(error = message(e)) }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Throwable) {
            _state.update { it.copy(error = message(e)) }
        }
    }

    private suspend fun loadState(): DeviceKeysState {
        val device = device()
        val view = api.deviceList(userId)
        var list: JsonObject? = view.deviceList
        var trusted = false
        var listedHere = true
        var linked = false
        if (device != null) {
            linked = device.op("deviceList").ok
            if (linked && list != null) {
                val checked = device.op("verifyDeviceList", "deviceList" to list)
                if (!checked.ok) {
                    throw EngineException(checked.error ?: "bad_device_list", "This account's device list doesn't verify. Don't approve anything until you know why.")
                }
                listedHere = checked.bool("thisDeviceListed") != false
                val own = device.op("deviceList")
                trusted = own.ok && own.bool("trusted") == true
                list = (own["deviceList"] as? JsonObject) ?: list
            }
        }
        // rv-signal-v1 (builds with the Signal engine, the default): turn it on, declare it in the signed list, top keys
        // up. A failure here never blocks the rest of the screen; the next load tries again.
        if (com.redvaultsystems.redvaultmessage.BuildConfig.E2E_SIGNAL && device != null && linked && listedHere) {
            try {
                maintainSignal(device)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                // Logged nowhere on purpose: the message could name a key.
            }
        }
        val registered = api.listDevices()
        val enrollments = if (device != null && trusted) api.enrollments() else emptyList()
        val events = runCatching { api.events() }.getOrDefault(emptyList()).takeLast(8).reversed()
        val signed = list?.get("devices")?.jsonArray?.map { it.jsonObject }.orEmpty().map {
            val id = it["deviceId"]!!.jsonPrimitive.content
            val signal = list?.get("capabilities")?.jsonArray.orEmpty().any { c -> c.jsonObject["deviceId"]?.jsonPrimitive?.content == id }
            TrustedDevice(id, it["ed25519"]!!.jsonPrimitive.content.take(16) + "…", id == device?.deviceId, signal)
        }
        val waiting = registered.filter { !it.listed && it.deviceId != device?.deviceId }.map { w -> WaitingDevice(w.deviceId, enrollments.any { it.newDeviceId == w.deviceId }) }
        return DeviceKeysState(
            hasDevice = device != null,
            linked = linked,
            listExists = list != null,
            trusted = trusted,
            listedHere = listedHere,
            devices = signed,
            waiting = waiting,
            pendingReset = view.pendingReset,
            events = events,
            protection = if (device != null) device.protection() else "none",
            localEvents = storage.localEvents(userId),
        )
    }

    private fun run(action: suspend () -> Unit) {
        scope.launch {
            _state.update { it.copy(busy = true, error = null) }
            try {
                blocking { action() }
                load()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Throwable) {
                _state.update { it.copy(error = message(e)) }
            } finally {
                _state.update { it.copy(busy = false) }
            }
        }
    }

    private fun engineRunner() = engine ?: throw EngineException("no_engine", "This build has no encryption engine.")

    /** This phone becomes the first trusted device; the current key vouches for it. */
    fun setUp() = run {
        ensureLock.withLock {
            dropDevice()
            val key = legacyPrivateKey()
            try {
                setUpFirstDevice(userId, key, storage, engineRunner(), api)
            } finally {
                key.fill(0)
            }
            dropDevice()
            device()?.let { keepSignal(it) }
        }
    }

    // --- This phone is the new device ---

    fun startPairing() {
        dropDevice()
        joinJob?.cancel()
        joinJob = scope.launch {
            _state.update { it.copy(busy = true, error = null, joining = true, joiningCodes = null) }
            try {
                val pairing = blocking {
                    val fresh = startNewDevice(userId, storage, engineRunner(), api)
                    NewDevicePairing.begin(fresh, engineRunner())
                }
                newPairing = pairing
                load()
                val codes = blocking { pairing.codes() }
                _state.update { it.copy(joiningCodes = codes, busy = false) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Throwable) {
                _state.update { it.copy(error = message(e), joining = false, joiningCodes = null, busy = false) }
                newPairing?.let { blocking { runCatching { it.cancel(); it.device.forget() } } }
                newPairing = null
                load()
            }
        }
    }

    /** The numbers match. */
    fun confirmJoin() {
        val pairing = newPairing ?: return
        joinJob = scope.launch {
            _state.update { it.copy(busy = true, error = null) }
            try {
                blocking { pairing.confirm() }
                newPairing = null
                dropDevice()
                _state.update { it.copy(joining = false, joiningCodes = null) }
                load()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Throwable) {
                _state.update { it.copy(error = message(e)) }
            } finally {
                _state.update { it.copy(busy = false) }
            }
        }
    }

    /** The numbers didn't match, or the person gave up. */
    fun rejectJoin() {
        val pairing = newPairing
        joinJob?.cancel()
        newPairing = null
        dropDevice()
        scope.launch {
            _state.update { it.copy(joining = false, joiningCodes = null, busy = true) }
            if (pairing != null) blocking { runCatching { pairing.cancel(); pairing.device.forget() } }
            _state.update { it.copy(busy = false) }
            load()
        }
    }

    // --- QR code pairing: for two phones that can see each other ---

    /** This phone is the new device: registers keys, shows a code, and waits for a trusted phone to scan it. */
    fun startQrLink() {
        dropDevice()
        joinJob?.cancel()
        joinJob = scope.launch {
            _state.update { it.copy(busy = true, error = null, joining = true, qrOffer = null) }
            var fresh: V3Device? = null
            try {
                val offer = blocking {
                    val created = startNewDevice(userId, storage, engineRunner(), api)
                    fresh = created
                    qrLinkOffer(created)
                }
                _state.update { it.copy(qrOffer = offer, busy = false) }
                load()
                blocking { waitForQrLink(fresh!!) }
                dropDevice()
                _state.update { it.copy(qrOffer = null, joining = false) }
                load()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Throwable) {
                _state.update { it.copy(error = message(e), joining = false, qrOffer = null, busy = false) }
                fresh?.let { f -> blocking { runCatching { f.op("cancelLink"); f.forget() } } }
                dropDevice()
                load()
            }
        }
    }

    fun cancelQrLink() {
        joinJob?.cancel()
        dropDevice()
        scope.launch {
            _state.update { it.copy(joining = false, qrOffer = null, busy = true) }
            blocking { runCatching { device()?.let { d -> d.op("cancelLink"); d.forget() } } }
            dropDevice()
            _state.update { it.copy(busy = false) }
            load()
        }
    }

    /** This phone is trusted and scanned a code: links exactly that device. */
    fun approveScanned(text: String) {
        _state.update { it.copy(qrLinked = false) }
        run {
            approveScannedQr(device() ?: throw EngineException("no_device", "This phone has no device keys."), text)
            _state.update { it.copy(qrLinked = true) }
        }
    }

    fun clearQrLinked() {
        _state.update { it.copy(qrLinked = false) }
    }

    // --- This phone is a trusted device ---

    fun compareCodes(newDeviceId: String) {
        answerJob?.cancel()
        answerJob = scope.launch {
            _state.update { it.copy(busy = true, error = null) }
            try {
                val begun = blocking {
                    val device = device() ?: throw EngineException("no_device", "This phone has no device keys.")
                    val request = api.enrollments().firstOrNull { it.newDeviceId == newDeviceId } ?: throw EngineException("no_request", "That request is gone.")
                    TrustedPairing.begin(device, engineRunner(), request.id)
                }
                trustedPairing = begun.pairing
                _state.update { it.copy(answeringDeviceId = newDeviceId, answeringCodes = begun.codes) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Throwable) {
                _state.update { it.copy(error = message(e)) }
            } finally {
                _state.update { it.copy(busy = false) }
            }
        }
    }

    /** The numbers match: approve exactly the device compared with. */
    fun confirmAnswer() {
        val pairing = trustedPairing ?: return
        scope.launch {
            _state.update { it.copy(busy = true, error = null) }
            try {
                blocking { pairing.confirm() }
                trustedPairing = null
                _state.update { it.copy(answeringDeviceId = null, answeringCodes = null) }
                load()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Throwable) {
                _state.update { it.copy(error = message(e)) }
            } finally {
                _state.update { it.copy(busy = false) }
            }
        }
    }

    /** The numbers didn't match: nothing is approved. */
    fun rejectAnswer() {
        val pairing = trustedPairing
        trustedPairing = null
        answerJob?.cancel()
        scope.launch {
            _state.update { it.copy(answeringDeviceId = null, answeringCodes = null) }
            if (pairing != null) blocking { pairing.reject() }
            load()
        }
    }

    // --- Devices, recovery, resets ---

    fun revoke(deviceId: String) = run { revokeDevice(device() ?: return@run, deviceId) }

    /** Turns Signal off for one of this account's devices: a signed list change; contacts are told. */
    fun turnOffSignal(deviceId: String) = run { turnOffSignal(device() ?: return@run, deviceId) }

    /** Deletes a device that registered but was never approved. */
    fun removeUnapproved(deviceId: String) = run { api.removeDevice(deviceId) }

    fun createRecoveryKey() = run {
        val key = createRecoveryBackup(device() ?: return@run)
        _state.update { it.copy(recoveryKey = key) }
        dismissRecoveryNotice()
    }

    fun clearRecoveryKey() {
        _state.update { it.copy(recoveryKey = null) }
    }

    fun restore(recoveryKeyText: String) = run {
        dropDevice()
        restoreWithRecoveryKey(userId, recoveryKeyText, storage, engineRunner(), api)
        dropDevice()
    }

    /** Keys here but never approved, or this phone was removed from the trusted devices: delete them. */
    fun forgetThisPhone() = run {
        val device = device()
        dropDevice()
        if (device != null) {
            device.op("cancelLink")
            device.forget()
        } else {
            storage.wipe(userId)
        }
    }

    fun cancelReset() = run {
        val device = device() ?: return@run
        val pending = api.deviceList(userId).pendingReset ?: return@run
        val refusal = device.must("cancelReset", "pendingAccountKey" to pending.accountKey)
        api.cancelReset(refusal.string("signature"))
    }

    /**
     * The safety number over your account key and [contactUserId]'s, or null while either has no device keys.
     * Throws EngineException(account_key_changed) when their keys changed since this phone last saw them.
     */
    suspend fun safetyNumber(contactUserId: String, conversationId: String?): List<String>? = blocking {
        val device = device() ?: return@blocking null
        if (!api.enabled()) return@blocking null
        deviceKeySafetyNumber(device, contactUserId, conversationId)
    }

    /** The device-key safety number with its code and state (a changed key included), or null while either has no device keys. */
    suspend fun safety(contactUserId: String, conversationId: String?): DeviceKeySafety? = blocking {
        val device = device() ?: return@blocking null
        if (!api.enabled()) return@blocking null
        deviceKeySafety(device, contactUserId, conversationId)
    }

    /** Marks the number over [accountKey] verified (or not); returns the new state. */
    suspend fun setSafetyVerified(contactUserId: String, conversationId: String?, accountKey: String, verified: Boolean): DeviceKeySafety? = blocking {
        val device = device() ?: return@blocking null
        setContactVerified(device, contactUserId, accountKey, verified)
        deviceKeySafety(device, contactUserId, conversationId)
    }

    /** Trusts the contact's new account key after a key change; returns the new state. */
    suspend fun acceptNewKeys(contactUserId: String, conversationId: String?, accountKey: String, verified: Boolean): DeviceKeySafety? = blocking {
        val device = device() ?: return@blocking null
        acceptContactKey(device, contactUserId, accountKey, verified, conversationId)
        deviceKeySafety(device, contactUserId, conversationId)
    }

    /** The other person's device-key code, read by the camera: whether it matched, and the new state. */
    suspend fun scanSafetyCode(contactUserId: String, conversationId: String?, accountKey: String, qrText: String): Pair<Boolean, DeviceKeySafety?> = blocking {
        val device = device() ?: return@blocking false to null
        val matched = verifyContactQr(device, contactUserId, accountKey, qrText, conversationId)
        matched to deviceKeySafety(device, contactUserId, conversationId)
    }

    private companion object {
        const val ENSURE_EVERY_MS = 6L * 60 * 60 * 1000
        const val RECOVERY_NOTICE = "recovery-notice"
    }
}
