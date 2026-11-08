// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.CryptoException
import com.redvaultsystems.redvaultmessage.crypto.DriveCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileDecryptException
import com.redvaultsystems.redvaultmessage.crypto.ShareCrypto
import com.redvaultsystems.redvaultmessage.crypto.ShareEnvelope
import com.redvaultsystems.redvaultmessage.crypto.ShareOwnerBox
import com.redvaultsystems.redvaultmessage.crypto.SharePasswordParams
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.DriveMetaRequest
import com.redvaultsystems.redvaultmessage.data.api.ShareApi
import com.redvaultsystems.redvaultmessage.data.api.ShareIncomingDto
import com.redvaultsystems.redvaultmessage.data.api.ShareItemInput
import com.redvaultsystems.redvaultmessage.data.api.ShareItemsRequest
import com.redvaultsystems.redvaultmessage.data.api.ShareLinkDto
import com.redvaultsystems.redvaultmessage.data.api.ShareOwnerItemDto
import com.redvaultsystems.redvaultmessage.data.api.ShareRemoveItemsRequest
import com.redvaultsystems.redvaultmessage.data.api.TransferTargetDto
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.IOException
import java.io.OutputStream
import java.time.Instant
import java.util.UUID

/** A public share link as its owner sees it: what the Files key opens of it. */
data class OwnerShare(
    val link: ShareLinkDto,
    /** Null when this account's Files key can't open the owner's copy. */
    val box: ShareOwnerBox?,
    /** Empty when [box] is missing. */
    val url: String,
    /** "active", "expired" or "revoked". */
    val status: String,
) {
    val active: Boolean get() = status == "active"
}

data class ShareOptions(
    val permission: String = "viewer",
    val label: String = "",
    /** Empty: no password. */
    val password: String = "",
    val expiresAtMs: Long? = null,
    val maxDownloads: Long? = null,
    val maxFileSize: Long? = null,
    val allowance: Long? = null,
    val maxFiles: Long? = null,
)

/** A file someone uploaded through a Contributor link, waiting for review. */
data class IncomingFile(
    val dto: ShareIncomingDto,
    /** What the uploader sealed (name, mime, size, modified, sent, key, header, chunk, sha); null if it can't be opened here. */
    val meta: JsonObject?,
    val name: String,
    /** The uploader's folder path, if any ("Reports/2026"). */
    val folder: String,
)

data class ShareItemPlan(val itemId: String, val backupId: String, val meta: JsonObject)

data class ReconcileResult(val added: Int = 0, val updated: Int = 0, val removed: Int = 0)

/**
 * Public share links for Files on this phone (docs/public-share-links.md), a
 * port of the desktop's data/share_repository and the web's lib/shareOwner.ts:
 * making links, keeping a folder link's items in step with Files, passwords,
 * regenerating, and the files people upload through Contributor links.
 */
class ShareRepository(
    private val api: ShareApi,
    private val share: ShareCrypto,
    private val drive: DriveCrypto,
    private val files: FileCrypto,
    private val identity: SelfIdentity,
    httpClient: OkHttpClient,
    apiBase: String,
    /** Where the share page lives: ".../redvaultmessages/". */
    private val webAppBase: String,
) {
    private val transfer = ProxyTransfer(httpClient, apiBase, "/shares/incoming/")
    private val driveKey: ByteArray get() = drive.driveKey(identity.privateKey)

    fun decode(link: ShareLinkDto): OwnerShare {
        val box = runCatching { share.openOwnerBox(driveKey, Base64Codec.decode(link.ownerBox), Base64Codec.decode(link.ownerBoxNonce)) }.getOrNull()
        val url = box?.let { b -> share.fromFragment(b.secret)?.let { share.shareUrl(webAppBase, link.publicId, it) } } ?: ""
        return OwnerShare(link, box, url, if (link.revoked) "revoked" else if (link.expired) "expired" else "active")
    }

    fun decodeIncoming(dto: ShareIncomingDto): IncomingFile {
        var meta = runCatching {
            share.openContribution(share.uploadKeyPair(driveKey, Base64Codec.decode(dto.keySalt)), Base64Codec.decode(dto.sealedMeta ?: ""))
        }.getOrNull()
        // The server's hash and the sealed one must agree: nothing else can be swapped in.
        if (meta != null && meta["sha"]?.jsonPrimitive?.content != dto.ciphertextSha256) meta = null
        val full = meta?.get("name")?.jsonPrimitive?.content ?: ""
        val name = full.substringAfterLast('/')
        val folder = if (full.contains('/')) full.substringBeforeLast('/') else ""
        return IncomingFile(dto, meta, name.ifEmpty { "Encrypted file" }, folder)
    }

    data class Listing(val links: List<OwnerShare>, val incoming: List<IncomingFile>)

    suspend fun refresh(): Listing = mapErrors("Couldn't load your links. Try again.") {
        val links = apiCall(ApiClient.json) { api.list() }.links.map(::decode)
        val incoming = apiCall(ApiClient.json) { api.incoming() }.items.map(::decodeIncoming)
        Listing(links, incoming)
    }

    private fun passwordJson(params: SharePasswordParams?): JsonElement =
        params?.let { p -> buildJsonObject { put("salt", Base64Codec.encode(p.salt)); put("ops", p.ops); put("mem", p.mem) } } ?: JsonNull

    private fun opt(v: Long?): JsonElement = v?.let { JsonPrimitive(it) } ?: JsonNull

    suspend fun create(root: DriveEntry, entries: List<DriveEntry>, options: ShareOptions): OwnerShare {
        if (root.locked) throw UserFacingException("This file is locked with its own key or password. Unlock it first, then share it.")
        if (options.permission == "contributor" && root.isFile) throw UserFacingException("Only folders can take uploads.")
        return mapErrors("Couldn't make the link. Try again.") {
            val made = withContext(Dispatchers.Default) {
                val secret = share.newLinkSecret()
                val shareKey = share.newShareKey()
                val uploadSalt = share.newSalt()
                val params = if (options.password.isNotEmpty()) share.newPasswordParams() else null
                val keys = share.deriveKeys(secret, options.password.ifEmpty { null }, params)
                val rootItemId = UUID.randomUUID().toString()
                if (plan(root.id, rootItemId, entries, emptyList()).size > MAX_ITEMS) {
                    throw UserFacingException("A link can show up to 5,000 items. Share a smaller folder.")
                }
                val envelope = share.sealEnvelope(keys.wrap, ShareEnvelope(shareKey, share.uploadKeyPair(driveKey, uploadSalt).publicKey, rootItemId))
                val box = ShareOwnerBox(share.toFragment(secret), Base64Codec.encode(shareKey), rootItemId, options.label.ifBlank { root.name }, System.currentTimeMillis())
                val owner = share.sealOwnerBox(driveKey, box)
                buildJsonObject {
                    put("backupId", root.id)
                    put("permission", options.permission)
                    put("lookupVerifier", share.verifierOf(keys.lookup))
                    put("accessVerifier", share.verifierOf(keys.access))
                    put("password", passwordJson(params))
                    put("envelope", Base64Codec.encode(envelope.ciphertext))
                    put("envelopeNonce", Base64Codec.encode(envelope.nonce))
                    put("ownerBox", Base64Codec.encode(owner.ciphertext))
                    put("ownerBoxNonce", Base64Codec.encode(owner.nonce))
                    put("uploadKeySalt", Base64Codec.encode(uploadSalt))
                    put("expiresAt", options.expiresAtMs?.let { JsonPrimitive(Instant.ofEpochMilli(it).toString()) } ?: JsonNull)
                    put("maxDownloads", opt(options.maxDownloads))
                    put(
                        "upload",
                        if (options.permission == "contributor") {
                            buildJsonObject { put("maxFileSize", opt(options.maxFileSize)); put("allowance", opt(options.allowance)); put("maxFiles", opt(options.maxFiles)) }
                        } else JsonNull,
                    )
                }
            }
            val created = decode(apiCall(ApiClient.json) { api.create(made) })
            reconcile(created, entries, emptyList())
            created
        }
    }

    /** Brings a link's items in line with Files (adds, re-seals, removes). */
    suspend fun reconcile(s: OwnerShare, entries: List<DriveEntry>, known: List<ShareOwnerItemDto>? = null): ReconcileResult {
        val box = s.box ?: return ReconcileResult()
        if (!s.active) return ReconcileResult()
        return mapErrors("Couldn't update the link. Try again.") {
            val shareKey = Base64Codec.decode(box.key)
            val fetched = known ?: apiCall(ApiClient.json) { api.items(s.link.id) }.items
            val (upserts, removals, counts) = withContext(Dispatchers.Default) {
                val desired = plan(s.link.backupId, box.root, entries, fetched)
                if (desired.size > MAX_ITEMS) throw UserFacingException("A link can show up to 5,000 items. Share a smaller folder.")
                val current = fetched.associateBy { it.itemId }
                var added = 0
                var updated = 0
                val upserts = mutableListOf<ShareItemInput>()
                for (d in desired) {
                    val have = current[d.itemId]
                    if (have != null && have.backupId == d.backupId) {
                        val meta = share.openItem(shareKey, have.itemId, Base64Codec.decode(have.metaCiphertext), Base64Codec.decode(have.metaNonce))
                        if (meta == d.meta) continue
                        updated++
                    } else if (have != null) {
                        updated++
                    } else {
                        added++
                    }
                    val sealed = share.sealItem(shareKey, d.itemId, d.meta)
                    upserts += ShareItemInput(d.itemId, d.backupId, Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce))
                }
                val wanted = desired.map { it.itemId }.toSet()
                val removals = fetched.map { it.itemId }.filter { it !in wanted }
                Triple(upserts, removals, ReconcileResult(added, updated, removals.size))
            }
            removals.chunked(BATCH).forEach { batch -> apiCall(ApiClient.json) { api.removeItems(s.link.id, ShareRemoveItemsRequest(batch)) } }
            upserts.chunked(BATCH).forEach { batch -> apiCall(ApiClient.json) { api.upsertItems(s.link.id, ShareItemsRequest(s.link.keyEpoch, batch)) } }
            counts
        }
    }

    /** Every live link; one failing doesn't stop the rest. */
    suspend fun reconcileAll(shares: List<OwnerShare>, entries: List<DriveEntry>) {
        for (s in shares) {
            if (!s.active || s.box == null) continue
            try {
                reconcile(s, entries)
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) throw e
            } catch (e: Exception) {
                // Tried again on the next refresh.
            }
        }
    }

    /** Permission, expiry, limits (a JSON body as in ui/src/lib/api.ts ShareSettingsInput), and a new name if given. */
    suspend fun updateSettings(s: OwnerShare, settings: JsonObject, label: String?): OwnerShare = mapErrors("Couldn't save the link. Try again.") {
        val body = if (label != null && s.box != null && label != s.box.label) {
            val owner = share.sealOwnerBox(driveKey, s.box.copy(label = label))
            JsonObject(settings + mapOf("ownerBox" to JsonPrimitive(Base64Codec.encode(owner.ciphertext)), "ownerBoxNonce" to JsonPrimitive(Base64Codec.encode(owner.nonce))))
        } else settings
        decode(apiCall(ApiClient.json) { api.update(s.link.id, body) })
    }

    /** Sets, changes or (empty) removes the password; the share key and items stay. */
    suspend fun setPassword(s: OwnerShare, password: String): OwnerShare {
        val box = s.box ?: throw UserFacingException("This link can't be changed on this phone.")
        return mapErrors("Couldn't change the password. Try again.") {
            val body = withContext(Dispatchers.Default) {
                val params = if (password.isNotEmpty()) share.newPasswordParams() else null
                val secret = share.fromFragment(box.secret) ?: throw UserFacingException("This link can't be changed on this phone.")
                val keys = share.deriveKeys(secret, password.ifEmpty { null }, params)
                val envelope = share.sealEnvelope(
                    keys.wrap,
                    ShareEnvelope(Base64Codec.decode(box.key), share.uploadKeyPair(driveKey, Base64Codec.decode(s.link.uploadKeySalt)).publicKey, box.root),
                )
                buildJsonObject {
                    put("password", passwordJson(params))
                    put("accessVerifier", share.verifierOf(keys.access))
                    put("envelope", Base64Codec.encode(envelope.ciphertext))
                    put("envelopeNonce", Base64Codec.encode(envelope.nonce))
                }
            }
            decode(apiCall(ApiClient.json) { api.setPassword(s.link.id, body) })
        }
    }

    /** A new address, secret and share key; the old link stops working. */
    suspend fun regenerate(s: OwnerShare, password: String, entries: List<DriveEntry>): OwnerShare {
        val box = s.box ?: throw UserFacingException("This link can't be changed on this phone.")
        return mapErrors("Couldn't make a new link. Try again.") {
            val body = withContext(Dispatchers.Default) {
                val secret = share.newLinkSecret()
                val shareKey = share.newShareKey()
                val params = if (password.isNotEmpty()) share.newPasswordParams() else null
                val keys = share.deriveKeys(secret, password.ifEmpty { null }, params)
                val rootItemId = UUID.randomUUID().toString()
                val envelope = share.sealEnvelope(
                    keys.wrap,
                    ShareEnvelope(shareKey, share.uploadKeyPair(driveKey, Base64Codec.decode(s.link.uploadKeySalt)).publicKey, rootItemId),
                )
                val owner = share.sealOwnerBox(driveKey, box.copy(secret = share.toFragment(secret), key = Base64Codec.encode(shareKey), root = rootItemId))
                buildJsonObject {
                    put("lookupVerifier", share.verifierOf(keys.lookup))
                    put("accessVerifier", share.verifierOf(keys.access))
                    put("password", passwordJson(params))
                    put("envelope", Base64Codec.encode(envelope.ciphertext))
                    put("envelopeNonce", Base64Codec.encode(envelope.nonce))
                    put("ownerBox", Base64Codec.encode(owner.ciphertext))
                    put("ownerBoxNonce", Base64Codec.encode(owner.nonce))
                }
            }
            val next = decode(apiCall(ApiClient.json) { api.regenerate(s.link.id, body) })
            reconcile(next, entries, emptyList())
            next
        }
    }

    suspend fun revoke(s: OwnerShare): OwnerShare =
        mapErrors("Couldn't revoke the link. Try again.") { decode(apiCall(ApiClient.json) { api.revoke(s.link.id, JsonObject(emptyMap())) }) }

    suspend fun remove(s: OwnerShare) = mapErrors("Couldn't delete the link. Try again.") { apiCall(ApiClient.json) { api.remove(s.link.id) } }

    /** Downloads and decrypts an incoming file into [output]; nothing unverified is kept by callers on failure. */
    suspend fun downloadIncoming(file: IncomingFile, output: OutputStream) {
        val meta = file.meta ?: throw UserFacingException("This file can't be opened on this phone.")
        mapErrors("Download failed. Try again.") {
            val target = TransferTargetDto("proxy", "/shares/incoming/${file.dto.id}/content", "GET", emptyMap(), Instant.now().plusSeconds(3600).toString())
            withContext(Dispatchers.IO) {
                transfer.run(target) { client, url, headers -> client.newCall(Request.Builder().url(url).get().headers(headers).build()).execute() }
                    .use { response ->
                        val body = response.body ?: throw IOException("Empty response")
                        files.decrypt(
                            input = body.byteStream(),
                            output = output,
                            key = Base64Codec.decode(meta.str("key")),
                            header = Base64Codec.decode(meta.str("header")),
                            encryptedSize = file.dto.encryptedSize,
                            ciphertextSha256 = meta.str("sha"),
                        )
                    }
            }
        }
    }

    /** Moves an incoming file into Files (into [parent]), re-sealed with the Files key. No bytes move. */
    suspend fun acceptIncoming(file: IncomingFile, parent: String) {
        val meta = file.meta ?: throw UserFacingException("This file can't be opened on this phone.")
        mapErrors("Couldn't add the file. Try again.") {
            val json = buildJsonObject {
                put("v", 1)
                put("kind", "file")
                put("name", file.name)
                put("parent", parent)
                put("modified", System.currentTimeMillis())
                put("trashed", false)
                put("trashedAt", 0)
                put("mime", meta.str("mime"))
                put("size", meta.getValue("size").jsonPrimitive.long)
                put("fileId", UUID.randomUUID().toString())
                put("version", 1)
                put("key", meta.str("key"))
                put("header", meta.str("header"))
                put("chunk", meta.getValue("chunk").jsonPrimitive.long)
                put("sha", meta.str("sha"))
            }
            val sealed = drive.sealMeta(json.toString(), driveKey)
            apiCall(ApiClient.json) { api.accept(file.dto.id, DriveMetaRequest(Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce))) }
        }
    }

    suspend fun rejectIncoming(file: IncomingFile) =
        mapErrors("Couldn't reject the file. Try again.") { apiCall(ApiClient.json) { api.reject(file.dto.id, JsonObject(emptyMap())) } }

    private suspend fun <T> mapErrors(fallback: String, block: suspend () -> T): T {
        try {
            return block()
        } catch (e: CancellationException) {
            throw e
        } catch (e: UserFacingException) {
            throw e
        } catch (e: FileDecryptException) {
            throw UserFacingException("Unable to decrypt this file. It may be corrupted or tampered with.")
        } catch (e: ApiException) {
            if (e.status == 401) throw e
            throw UserFacingException(
                when {
                    // The server's own wording for the link rules is written to be shown.
                    e.code.startsWith("share_") || e.code in SERVER_WORDED -> e.message ?: fallback
                    e.code == "backup_disabled" -> "Files aren't available on this server."
                    e.code == "network_error" -> "Network interrupted. Try again."
                    e.status == 404 -> "That isn't there anymore. Refresh and try again."
                    else -> fallback
                },
            )
        } catch (e: CryptoException) {
            throw UserFacingException(fallback)
        } catch (e: IOException) {
            throw UserFacingException("Network interrupted. Try again.")
        } catch (e: kotlinx.serialization.SerializationException) {
            throw UserFacingException(fallback)
        } catch (e: IllegalArgumentException) {
            throw UserFacingException(fallback)
        }
    }

    companion object {
        /** A folder link shows at most this many items (the server enforces it too). */
        const val MAX_ITEMS = 5000
        private const val BATCH = 200
        private val SERVER_WORDED = setOf("validation_error", "backup_storage_full", "rate_limited")

        private fun JsonObject.str(key: String): String = this[key]?.jsonPrimitive?.content ?: ""

        /**
         * What a link should show: the shared file, or the folder and
         * everything in it outside the Trash, with the newest version of each
         * file. Locked files are left out. Item ids already in the link are kept.
         */
        fun plan(rootBackupId: String, rootItemId: String, entries: List<DriveEntry>, existing: List<ShareOwnerItemDto>): List<ShareItemPlan> {
            val byId = entries.associateBy { it.id }
            val root = byId[rootBackupId] ?: return emptyList()
            if (root.trashed) return emptyList()
            val idByBackup = existing.associate { it.backupId to it.itemId }
            val idByFileId = HashMap<String, String>()
            for (i in existing) byId[i.backupId]?.takeIf { it.isFile }?.let { idByFileId[it.fileId] = i.itemId }

            fun fileMeta(e: DriveEntry, parent: String): JsonObject? {
                val key = (e.meta["key"] as? JsonPrimitive)?.takeIf { it.isString }?.content ?: return null
                val header = (e.meta["header"] as? JsonPrimitive)?.takeIf { it.isString }?.content ?: return null
                val sha = (e.meta["sha"] as? JsonPrimitive)?.takeIf { it.isString }?.content ?: return null
                val chunk = (e.meta["chunk"] as? JsonPrimitive)?.content?.toLongOrNull() ?: FileCrypto.CHUNK_BYTES.toLong()
                return ShareCrypto.fileMeta(e.name, parent, e.mimeType, e.size, maxOf(0L, e.modifiedMs), key, header, chunk, sha)
            }
            if (root.isFile) return listOfNotNull(fileMeta(root, "")?.let { ShareItemPlan(rootItemId, root.id, it) })

            val out = mutableListOf(ShareItemPlan(rootItemId, root.id, ShareCrypto.folderMeta(root.name, "")))
            val children = entries.filter { !it.trashed && !it.incoming }.groupBy { it.parent }
            val visited = hashSetOf(root.id)
            val queue = ArrayDeque(listOf(Triple(root, rootItemId, 0)))
            while (queue.isNotEmpty()) {
                val (folder, itemId, depth) = queue.removeFirst()
                val newest = LinkedHashMap<String, DriveEntry>()
                for (e in children[folder.id].orEmpty()) {
                    if (!e.isFile) {
                        if (e.id in visited || depth >= 64) continue
                        visited += e.id
                        val childId = idByBackup[e.id] ?: UUID.randomUUID().toString()
                        out += ShareItemPlan(childId, e.id, ShareCrypto.folderMeta(e.name, itemId))
                        queue.addLast(Triple(e, childId, depth + 1))
                    } else {
                        val seen = newest[e.fileId]
                        if (seen == null || e.version > seen.version || (e.version == seen.version && e.modifiedMs > seen.modifiedMs)) newest[e.fileId] = e
                    }
                }
                for ((fileId, e) in newest) {
                    val meta = fileMeta(e, itemId) ?: continue
                    out += ShareItemPlan(idByFileId[fileId] ?: idByBackup[e.id] ?: UUID.randomUUID().toString(), e.id, meta)
                }
                if (out.size > MAX_ITEMS + 1) break
            }
            return out
        }
    }
}
