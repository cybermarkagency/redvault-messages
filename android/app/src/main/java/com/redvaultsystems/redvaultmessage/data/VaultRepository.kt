package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.CryptoException
import com.redvaultsystems.redvaultmessage.crypto.VaultCrypto
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.DriveApi
import com.redvaultsystems.redvaultmessage.data.api.VaultItemDto
import com.redvaultsystems.redvaultmessage.data.api.VaultItemRequest
import com.redvaultsystems.redvaultmessage.data.api.VaultShareRequest
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import java.io.IOException

/**
 * One password-manager item, decrypted on this phone. [data] holds its
 * fields as JSON: {"type": "login"|"passkey"|"card"|"note"|"wifi"|"filekey",
 * "title", "username", "password", "urls": [...], "notes", "totp", ...}.
 */
data class VaultEntry(
    val id: String = "",
    val revision: Int = 0,
    val data: JsonObject,
    /** Someone else's item shared with this account (read-only). */
    val incoming: Boolean = false,
    val ownerId: String = "",
    /** Whom the owner shares it with (user ids). Only for the owner's own items. */
    val sharedWith: List<String> = emptyList(),
) {
    val type: String get() = data.stringOr("type", "login")
}

/** The highest revision of each vault item this phone has seen, per account. */
interface VaultRevs {
    fun seen(userId: String, itemId: String): Long
    fun record(userId: String, itemId: String, rev: Long)
}

class VaultRevStore(context: Context) : VaultRevs {
    private val prefs = context.getSharedPreferences("vaultRevs", Context.MODE_PRIVATE)

    override fun seen(userId: String, itemId: String) = prefs.getLong("$userId:$itemId", 0)

    override fun record(userId: String, itemId: String, rev: Long) {
        prefs.edit().putLong("$userId:$itemId", rev).apply()
    }
}

/**
 * The password manager's storage (a port of the desktop app's
 * VaultRepository): seals items under the vault key before they go to the
 * server, opens them after, keeps shared copies in step with edits, and
 * asks the private breach check.
 */
class VaultRepository(
    private val api: DriveApi,
    private val users: ApiService,
    private val crypto: VaultCrypto,
    private val identity: SelfIdentity,
    private val revs: VaultRevs,
    private val contactKeys: ContactKeyPins? = null,
) {
    private val json = Json { ignoreUnknownKeys = true }

    private val key: ByteArray get() = crypto.vaultKey(identity.privateKey)

    /** How many items the last [list] left out, because they aren't what this phone last saw. */
    @Volatile var refused = 0
        private set

    /**
     * False for an item in another record's place, or older than this phone has seen: `boundId` and `rev` are inside
     * the sealed data, so the server can't swap two records or hand back an older copy. Items are bound when next saved.
     */
    private fun accept(id: String, data: JsonObject, shared: Boolean = false): Boolean {
        val record = id.lowercase()
        val bound = data["boundId"]?.jsonPrimitive?.contentOrNull
        if (bound != null && bound.lowercase() != record) return false
        val name = if (shared) "shared:$record" else record
        val rev = data["rev"]?.jsonPrimitive?.longOrNull ?: 0
        val seen = revs.seen(identity.userId.lowercase(), name)
        if (rev < seen) return false
        if (rev > seen) revs.record(identity.userId.lowercase(), name, rev)
        return true
    }

    /** The key to seal someone's copy to, or null while their safety number is changed: it waits until that's checked. */
    private fun sealable(recipientId: String, publicKeyB64: String): ByteArray? =
        if (contactKeys?.keyChanged(identity.userId, recipientId, publicKeyB64, verified = false) == true) null else Base64Codec.decode(publicKeyB64)

    /** Own items, then items shared with this account. */
    suspend fun list(): List<VaultEntry> = mapErrors("Couldn't load your vault. Try again.") {
        val key = key
        var refusedNow = 0
        val sharedWith = apiCall(ApiClient.json) { api.vaultShares() }.shares.groupBy({ it.itemId }, { it.recipientId })
        val own = apiCall(ApiClient.json) { api.vaultItems() }.items.mapNotNull { item ->
            runCatching {
                VaultEntry(
                    id = item.id,
                    revision = item.revision,
                    data = json.parseToJsonElement(crypto.openItem(Base64Codec.decode(item.ciphertext), Base64Codec.decode(item.nonce), key)).jsonObject,
                    sharedWith = sharedWith[item.id].orEmpty(),
                )
            }.getOrNull() // sealed under another key (never expected): left out rather than shown broken
                ?.takeIf { entry -> accept(entry.id, entry.data).also { if (!it) refusedNow++ } }
        }
        val incoming = apiCall(ApiClient.json) { api.vaultSharedWithMe() }.items.mapNotNull { shared ->
            runCatching {
                VaultEntry(
                    id = shared.itemId,
                    incoming = true,
                    ownerId = shared.ownerId,
                    data = json.parseToJsonElement(
                        crypto.openFromSender(Base64Codec.decode(shared.sealed), identity.publicKey, identity.privateKey),
                    ).jsonObject,
                )
            }.getOrNull()?.takeIf { entry -> accept(entry.id, entry.data, shared = true).also { if (!it) refusedNow++ } }
        }
        key.fill(0)
        refused = refusedNow
        own + incoming
    }

    /** Creates the item (empty id) or saves a change, refreshing everyone's shared copy. */
    suspend fun save(entry: VaultEntry): VaultEntry = mapErrors("Couldn't save the item. Try again.") {
        if (entry.incoming) throw UserFacingException("Items shared with you are read-only.")
        val now = System.currentTimeMillis()
        val data = entry.data.toMutableMap()
        if (!data.containsKey("created")) data["created"] = JsonPrimitive(now)
        data["updated"] = JsonPrimitive(now)
        data["v"] = JsonPrimitive(1)
        val key = key
        val request = { fields: Map<String, JsonElement> ->
            val sealed = crypto.sealItem(JsonObject(fields).toString(), key)
            VaultItemRequest(Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce))
        }
        var stored: VaultItemDto
        try {
            if (entry.id.isEmpty()) {
                // The server picks the id, so the item is bound in a second write. If that one fails, its next save does it.
                val created = apiCall(ApiClient.json) { api.createVaultItem(request(data)) }
                val bound = data + mapOf("boundId" to JsonPrimitive(created.id), "rev" to JsonPrimitive(1))
                stored = try {
                    apiCall(ApiClient.json) { api.updateVaultItem(created.id, request(bound).copy(revision = created.revision)) }.also { data.putAll(bound) }
                } catch (_: ApiException) {
                    created
                } catch (_: IOException) {
                    created
                }
            } else {
                data["boundId"] = JsonPrimitive(entry.id)
                data["rev"] = JsonPrimitive((entry.data["rev"]?.jsonPrimitive?.longOrNull ?: 0) + 1)
                stored = apiCall(ApiClient.json) { api.updateVaultItem(entry.id, request(data).copy(revision = entry.revision)) }
            }
        } finally {
            key.fill(0)
        }
        accept(stored.id, JsonObject(data))
        val text = JsonObject(data).toString()
        for (recipientId in entry.sharedWith) {
            val recipient = apiCall(ApiClient.json) { users.getUser(recipientId) }
            val publicKey = sealable(recipientId, recipient.publicKey) ?: continue
            val copy = crypto.sealForRecipient(text, publicKey)
            apiCall(ApiClient.json) { api.shareVaultItem(stored.id, recipientId, VaultShareRequest(Base64Codec.encode(copy))) }
        }
        entry.copy(id = stored.id, revision = stored.revision, data = JsonObject(data))
    }

    suspend fun remove(entry: VaultEntry) = mapErrors("Couldn't delete the item. Try again.") {
        apiCall(ApiClient.json) { api.deleteVaultItem(entry.id) }
    }

    suspend fun share(entry: VaultEntry, recipientId: String) = mapErrors("Couldn't share the item. Try again.") {
        val recipient = apiCall(ApiClient.json) { users.getUser(recipientId) }
        val publicKey = sealable(recipientId, recipient.publicKey) ?: throw UserFacingException(KeyTrust.CONTACT_KEY_CHANGED)
        val copy = crypto.sealForRecipient(entry.data.toString(), publicKey)
        apiCall(ApiClient.json) { api.shareVaultItem(entry.id, recipientId, VaultShareRequest(Base64Codec.encode(copy))) }
    }

    suspend fun unshare(entry: VaultEntry, recipientId: String) = mapErrors("Couldn't stop sharing the item. Try again.") {
        apiCall(ApiClient.json) { api.unshareVaultItem(entry.id, recipientId) }
    }

    /**
     * How many times this password appears in known breaches (0: none),
     * checked privately: only the first 5 characters of its SHA-1 leave the
     * phone. -1 when the check can't be reached.
     */
    suspend fun breachCount(password: String): Long {
        if (password.isEmpty()) return 0
        return try {
            val hash = crypto.breachHash(password)
            val range = apiCall(ApiClient.json) { api.breachRange(hash.substring(0, 5)).string() }
            crypto.breachCount(hash, range)
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) throw e
            -1
        } catch (e: Exception) {
            -1
        }
    }

    private suspend fun <T> mapErrors(fallback: String, block: suspend () -> T): T {
        try {
            return block()
        } catch (e: CancellationException) {
            throw e
        } catch (e: UserFacingException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) throw e
            throw UserFacingException(
                when {
                    e.code == "vault_conflict" -> "This item was changed on another device. It's been reloaded: make your change again."
                    e.code == "network_error" -> "Network interrupted. Try again."
                    e.code == "rate_limited" -> e.message ?: fallback
                    e.status == 404 -> "That item isn't there anymore."
                    else -> fallback
                },
            )
        } catch (e: IOException) {
            throw UserFacingException("Network interrupted. Try again.")
        } catch (e: CryptoException) {
            throw UserFacingException(fallback)
        } catch (e: kotlinx.serialization.SerializationException) {
            throw UserFacingException(fallback)
        }
    }
}
