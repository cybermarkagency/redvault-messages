// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.passwords

import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.GeneratorOptions
import com.redvaultsystems.redvaultmessage.crypto.Strength
import com.redvaultsystems.redvaultmessage.crypto.VaultCrypto
import com.redvaultsystems.redvaultmessage.data.FileKeyRepository
import com.redvaultsystems.redvaultmessage.data.UserFacingException
import com.redvaultsystems.redvaultmessage.data.VaultEntry
import com.redvaultsystems.redvaultmessage.data.VaultRepository
import com.redvaultsystems.redvaultmessage.data.api.ApiException
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
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import java.net.URI
import java.security.MessageDigest

/** Why an item shows up in the security check. Exposed wins over reused, reused over weak. */
enum class ItemStatus { None, Exposed, Reused, Weak }

/** One row of the Passwords list. */
data class VaultRow(val entry: VaultEntry, val status: ItemStatus = ItemStatus.None)

data class VaultState(
    val locked: Boolean = false,
    val loaded: Boolean = false,
    val rows: List<VaultRow> = emptyList(),
    /** Whether every own password has been through the private breach check yet. */
    val breachChecked: Boolean = false,
    /** The breach check couldn't be reached (or is off on this server) for at least one password. */
    val breachUnavailable: Boolean = false,
    val busy: Boolean = false,
    val error: String? = null,
    val notice: String? = null,
) {
    fun find(id: String): VaultRow? = rows.firstOrNull { it.entry.id == id }
    val exposed: Int get() = rows.count { it.status == ItemStatus.Exposed }
    val reused: Int get() = rows.count { it.status == ItemStatus.Reused }
    val weak: Int get() = rows.count { it.status == ItemStatus.Weak }
}

/** The fields of the item editor (one flat set for every type; each type uses some). */
data class ItemFields(
    val id: String = "",
    val type: String = "login",
    val title: String = "",
    val username: String = "",
    val password: String = "",
    val url: String = "",
    val notes: String = "",
    val totp: String = "",
    val cardHolder: String = "",
    val cardNumber: String = "",
    val cardExpiry: String = "",
    val cardCvv: String = "",
    val wifiSsid: String = "",
    val wifiSecurity: String = "",
    val wifiPassword: String = "",
    val passkeyRpId: String = "",
    val passkeyUser: String = "",
)

/**
 * The password manager for one signed-in session (a port of the desktop
 * app's VaultSession). It locks after [IDLE_LOCK_MS] without use, and when
 * locked keeps no item data in memory. Unlocking takes the account
 * password (checked on this phone) or the phone's fingerprint / face.
 * File keys live in the same vault but belong to Files, so they're left
 * off this list.
 */
class VaultSession(
    private val repository: VaultRepository,
    private val crypto: VaultCrypto,
    private val e2e: E2eCrypto,
    private val identity: SelfIdentity,
    private val onSessionExpired: () -> Unit,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow(VaultState())
    val state: StateFlow<VaultState> = _state.asStateFlow()

    private var idleJob: Job? = null
    private var loadJob: Job? = null
    /** Private breach check results by password hash, kept only while unlocked. */
    private val breachResults = HashMap<String, Long>()

    init {
        load()
        touch()
    }

    // --- Lock ---

    /** Restarts the idle timer. Call on any interaction with the vault. */
    fun touch() {
        if (_state.value.locked) return
        idleJob?.cancel()
        idleJob = scope.launch {
            delay(IDLE_LOCK_MS)
            lockNow()
        }
    }

    /** Locks now: nothing readable stays in memory. */
    fun lockNow() {
        if (_state.value.locked) return
        idleJob?.cancel()
        loadJob?.cancel()
        breachResults.clear()
        _state.value = VaultState(locked = true)
    }

    /** Checks the account password on this phone, the way the key was unwrapped at sign-in. */
    suspend fun unlockWithPassword(password: String): Boolean {
        if (!_state.value.locked) return true
        if (password.isEmpty()) {
            _state.update { it.copy(error = "Enter your password.") }
            return false
        }
        _state.update { it.copy(busy = true, error = null) }
        val matches = withContext(Dispatchers.Default) {
            val wrapped = identity.wrappedKey ?: return@withContext false
            runCatching {
                val unwrapped = e2e.unwrapPrivateKey(wrapped, password)
                val same = MessageDigest.isEqual(unwrapped, identity.privateKey)
                unwrapped.fill(0)
                same
            }.getOrDefault(false)
        }
        _state.update { it.copy(busy = false) }
        if (!matches) {
            _state.update { it.copy(error = "That password isn't right.") }
            return false
        }
        unlocked()
        return true
    }

    /** After the phone confirmed the owner's fingerprint or face. */
    fun unlockedWithBiometrics() {
        if (_state.value.locked) unlocked()
    }

    private fun unlocked() {
        _state.value = VaultState(locked = false)
        load()
        touch()
    }

    // --- Loading ---

    fun refresh() {
        if (!_state.value.locked) load()
    }

    private fun load() {
        loadJob?.cancel()
        loadJob = scope.launch {
            try {
                val entries = repository.list().filterNot { FileKeyRepository.isFileKeyItem(it.data) }
                if (_state.value.locked) return@launch
                val refused = repository.refused.takeIf { it > 0 }?.let {
                    (if (it == 1) "One item isn't shown" else "$it items aren't shown") +
                        ": they don't match what this phone last saw, so they may have been changed on the server."
                }
                _state.update { it.copy(loaded = true, rows = statuses(entries.map { e -> VaultRow(e) }), error = refused) }
                checkBreaches()
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired() else _state.update { it.copy(error = e.message) }
            } catch (e: UserFacingException) {
                _state.update { it.copy(error = e.message) }
            }
        }
    }

    /** Exposed (from the breach check), reused (same password on two of your items), weak. */
    private fun statuses(rows: List<VaultRow>): List<VaultRow> {
        val uses = HashMap<String, Int>()
        rows.filter { !it.entry.incoming }.forEach { row ->
            secretOf(row.entry.data).takeIf { it.isNotEmpty() }?.let { uses[it] = (uses[it] ?: 0) + 1 }
        }
        return rows.map { row ->
            val secret = secretOf(row.entry.data)
            val status = when {
                secret.isEmpty() || row.entry.incoming -> ItemStatus.None
                (breachResults[crypto.breachHash(secret)] ?: 0) > 0 -> ItemStatus.Exposed
                (uses[secret] ?: 0) > 1 -> ItemStatus.Reused
                crypto.estimateStrength(secret).strength == Strength.Weak -> ItemStatus.Weak
                else -> ItemStatus.None
            }
            row.copy(status = status)
        }
    }

    /** The private breach check for each password not yet checked this unlock (only a 5-character hash prefix leaves the phone). */
    private suspend fun checkBreaches() {
        val secrets = _state.value.rows.filter { !it.entry.incoming }.map { secretOf(it.entry.data) }.filter { it.isNotEmpty() }.distinct()
        var allChecked = true
        for (secret in secrets) {
            val hash = crypto.breachHash(secret)
            if (breachResults.containsKey(hash)) continue
            val count = try {
                repository.breachCount(secret)
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired()
                -1
            }
            if (_state.value.locked) return
            if (count < 0) allChecked = false else breachResults[hash] = count
        }
        _state.update { it.copy(rows = statuses(it.rows), breachChecked = allChecked, breachUnavailable = !allChecked) }
    }

    // --- Items ---

    fun fieldsFor(id: String): ItemFields? {
        val row = _state.value.find(id) ?: return null
        if (row.entry.incoming) return null
        val d = row.entry.data
        return ItemFields(
            id = row.entry.id,
            type = d.str("type").ifEmpty { "login" },
            title = d.str("title"),
            username = d.str("username"),
            password = d.str("password"),
            url = firstUrl(d),
            notes = d.str("notes"),
            totp = d.str("totp"),
            cardHolder = d.nested("card", "holder"),
            cardNumber = d.nested("card", "number"),
            cardExpiry = d.nested("card", "expiry"),
            cardCvv = d.nested("card", "cvv"),
            wifiSsid = d.nested("wifi", "ssid"),
            wifiSecurity = d.nested("wifi", "security"),
            wifiPassword = d.nested("wifi", "password"),
            passkeyRpId = d.nested("passkey", "rpId"),
            passkeyUser = d.nested("passkey", "userName"),
        )
    }

    fun blankFields(type: String): ItemFields =
        ItemFields(type = type, password = if (type == "login") generate(GeneratorOptions()) else "", wifiPassword = if (type == "wifi") generate(GeneratorOptions()) else "")

    /** Saves new or edited fields; returns the saved item's id, or null with the error in the state. */
    suspend fun save(fields: ItemFields): String? {
        touch()
        if (fields.title.isBlank()) {
            _state.update { it.copy(error = "Give it a name.") }
            return null
        }
        if (fields.totp.isNotBlank() && crypto.parseTotp(fields.totp.trim()) == null) {
            _state.update { it.copy(error = "That one-time code setup key isn't valid. Paste the key or otpauth:// link from the site.") }
            return null
        }
        val existing = _state.value.find(fields.id)?.entry
        val old = existing?.data ?: JsonObject(emptyMap())
        val type = fields.type
        val data = buildJsonObject {
            // Keep fields this app doesn't edit (another app may have written them).
            old.forEach { (k, v) -> if (k !in EDITED_KEYS) put(k, v) }
            put("type", type)
            put("title", fields.title.trim())
            put("username", fields.username.trim())
            put("password", fields.password)
            put("notes", fields.notes)
            put("totp", fields.totp.trim())
            put("urls", JsonArray(if (fields.url.isBlank()) emptyList() else listOf(JsonPrimitive(fields.url.trim()))))
            if (type == "card") {
                put("card", buildJsonObject {
                    put("holder", fields.cardHolder.trim()); put("number", fields.cardNumber.trim())
                    put("expiry", fields.cardExpiry.trim()); put("cvv", fields.cardCvv.trim())
                })
            } else old["card"]?.let { put("card", it) }
            if (type == "wifi") {
                put("wifi", buildJsonObject {
                    put("ssid", fields.wifiSsid.trim()); put("security", fields.wifiSecurity.trim()); put("password", fields.wifiPassword)
                })
            } else old["wifi"]?.let { put("wifi", it) }
            // A passkey can be saved on its own or alongside a login's password.
            if (type == "passkey" || fields.passkeyRpId.isNotBlank()) {
                put("passkey", buildJsonObject {
                    put("rpId", fields.passkeyRpId.trim())
                    put("userName", fields.passkeyUser.trim().ifEmpty { fields.username.trim() })
                })
            }
            val changed = secretOf(old) != (if (type == "wifi") fields.wifiPassword else fields.password)
            val passwordChanged = if (changed || existing == null) System.currentTimeMillis() else old.long("passwordChanged") ?: old.long("updated")
            passwordChanged?.let { put("passwordChanged", it) }
        }
        val entry = existing?.copy(data = data) ?: VaultEntry(data = data)
        _state.update { it.copy(busy = true, error = null, notice = null) }
        return try {
            val stored = repository.save(entry)
            if (_state.value.locked) return null
            _state.update { s ->
                val rows = if (s.rows.any { it.entry.id == stored.id }) s.rows.map { if (it.entry.id == stored.id) VaultRow(stored) else it } else s.rows + VaultRow(stored)
                s.copy(rows = statuses(rows), busy = false, notice = "Saved.")
            }
            scope.launch { checkBreaches() }
            stored.id
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (e is ApiException && e.status == 401) onSessionExpired()
            _state.update { it.copy(busy = false, error = e.message ?: "Couldn't save the item. Try again.") }
            null
        }
    }

    fun delete(id: String) = act("Deleted.") {
        val row = _state.value.find(id) ?: return@act
        if (row.entry.incoming) return@act
        repository.remove(row.entry)
        _state.update { s -> s.copy(rows = statuses(s.rows.filterNot { it.entry.id == id })) }
    }

    fun share(id: String, userId: String, handle: String) = act("Shared with $handle. Only they can open it.") {
        val row = _state.value.find(id) ?: return@act
        repository.share(row.entry, userId)
        updateSharedWith(id) { if (userId in it) it else it + userId }
    }

    fun unshare(id: String, userId: String) = act(null) {
        val row = _state.value.find(id) ?: return@act
        repository.unshare(row.entry, userId)
        updateSharedWith(id) { it - userId }
    }

    private fun updateSharedWith(id: String, change: (List<String>) -> List<String>) {
        _state.update { s -> s.copy(rows = s.rows.map { if (it.entry.id == id) it.copy(entry = it.entry.copy(sharedWith = change(it.entry.sharedWith))) else it }) }
    }

    // --- Reading items ---

    fun secretOf(id: String): String = _state.value.find(id)?.let { secretOf(it.entry.data) }.orEmpty()

    fun field(id: String, name: String): String {
        val d = _state.value.find(id)?.entry?.data ?: return ""
        return when (name) {
            "cardNumber" -> d.nested("card", "number")
            "cardCvv" -> d.nested("card", "cvv")
            "website" -> firstUrl(d)
            else -> d.str(name)
        }
    }

    /** The current one-time code ("482 913") and seconds left, or null when the item has none. */
    fun totp(id: String, nowSeconds: Long): Triple<String, Int, Int>? {
        val secret = _state.value.find(id)?.entry?.data?.str("totp").orEmpty()
        if (secret.isEmpty()) return null
        val params = crypto.parseTotp(secret) ?: return null
        val code = crypto.totpCode(params, nowSeconds)
        val shown = if (code.length == 6) code.substring(0, 3) + " " + code.substring(3) else code
        return Triple(shown, crypto.totpSecondsLeft(params, nowSeconds), params.period)
    }

    fun strength(password: String) = if (password.isEmpty()) null else crypto.estimateStrength(password)

    fun generate(options: GeneratorOptions): String = crypto.generatePassword(options)

    fun reusedWith(id: String): Int {
        val secret = secretOf(id)
        if (secret.isEmpty()) return 0
        return _state.value.rows.count { it.entry.id != id && !it.entry.incoming && secretOf(it.entry.data) == secret }
    }

    fun clearMessages() = _state.update { it.copy(error = null, notice = null) }

    fun notice(text: String) = _state.update { it.copy(notice = text, error = null) }

    private fun act(done: String?, block: suspend () -> Unit) {
        touch()
        scope.launch {
            _state.update { it.copy(busy = true, error = null, notice = null) }
            try {
                block()
                _state.update { it.copy(busy = false, notice = done) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                if (e is ApiException && e.status == 401) onSessionExpired()
                _state.update { it.copy(busy = false, error = e.message ?: "Something went wrong. Try again.") }
            }
        }
    }

    fun close() {
        scope.cancel()
        breachResults.clear()
        _state.value = VaultState(locked = true)
    }

    companion object {
        const val IDLE_LOCK_MS = 5 * 60 * 1000L
        private val EDITED_KEYS = setOf(
            "type", "title", "username", "password", "notes", "totp", "urls", "card", "wifi", "passkey", "passwordChanged",
        )

        /** The password an item's security check is about (logins and Wi-Fi). */
        fun secretOf(d: JsonObject): String = if (d.str("type") == "wifi") d.nested("wifi", "password") else d.str("password")

        fun firstUrl(d: JsonObject): String = (d["urls"] as? JsonArray)?.firstOrNull()?.let { runCatching { it.jsonPrimitive.contentOrNull }.getOrNull() }.orEmpty()

        fun hostOf(url: String): String {
            if (url.isBlank()) return ""
            val withScheme = if ("://" in url) url else "https://$url"
            return runCatching { URI(withScheme).host }.getOrNull()?.removePrefix("www.") ?: url
        }

        internal fun JsonObject.str(key: String): String = (this[key] as? JsonPrimitive)?.takeIf { it.isString }?.content.orEmpty()

        internal fun JsonObject.nested(obj: String, key: String): String = (this[obj] as? JsonObject)?.str(key).orEmpty()

        internal fun JsonObject.long(key: String): Long? = (this[key] as? JsonPrimitive)?.let { runCatching { it.content.toLong() }.getOrNull() }
    }
}

/** A row's subtitle, as on the desktop: username and site, card expiry and last digits, Wi-Fi network, or a note's first line. */
fun subtitleOf(entry: VaultEntry): String {
    val d = entry.data
    with(VaultSession) {
        return when (d.str("type")) {
            "card" -> {
                val number = d.nested("card", "number")
                "Expires ${d.nested("card", "expiry")} · Card ending ${number.takeLast(4)}"
            }
            "wifi" -> "${d.nested("wifi", "security").ifEmpty { "Wi-Fi" }} · Network: ${d.nested("wifi", "ssid")}"
            "note" -> d.str("notes").lineSequence().firstOrNull().orEmpty().take(60)
            else -> {
                val parts = listOfNotNull(d.str("username").ifEmpty { null }, hostOf(firstUrl(d)).ifEmpty { null })
                val passkey = if (d.nested("passkey", "rpId").isNotEmpty()) " · passkey" else ""
                parts.joinToString(" · ") + passkey
            }
        }
    }
}

internal fun titleOf(entry: VaultEntry): String = with(VaultSession) { entry.data.str("title").ifEmpty { "Untitled" } }

internal fun hasPasskey(entry: VaultEntry): Boolean = with(VaultSession) { entry.data.nested("passkey", "rpId").isNotEmpty() || entry.data.str("type") == "passkey" }
