package com.redvaultsystems.redvaultmessage.data

import okhttp3.RequestBody.Companion.toRequestBody
import com.redvaultsystems.redvaultmessage.tor.TorUnavailableException
import com.redvaultsystems.redvaultmessage.data.api.TransferTargetDto
import com.redvaultsystems.redvaultmessage.data.api.TOR_UNAVAILABLE_MESSAGE
import com.redvaultsystems.redvaultmessage.data.api.DriveUploadPartsRequest
import android.content.ContentResolver
import android.net.Uri
import android.provider.OpenableColumns
import android.webkit.MimeTypeMap
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.CryptoException
import com.redvaultsystems.redvaultmessage.crypto.DriveCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileDecryptException
import com.redvaultsystems.redvaultmessage.crypto.VaultCrypto
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.DriveApi
import com.redvaultsystems.redvaultmessage.data.api.DriveEntryDto
import com.redvaultsystems.redvaultmessage.data.api.DriveLinkDto
import com.redvaultsystems.redvaultmessage.data.api.DriveLinkRequest
import com.redvaultsystems.redvaultmessage.data.api.DriveMetaRequest
import com.redvaultsystems.redvaultmessage.data.api.DriveShareRequest
import com.redvaultsystems.redvaultmessage.data.api.DriveUploadRequest
import com.redvaultsystems.redvaultmessage.data.api.DriveUsageDto
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.asRequestBody
import java.io.File
import java.io.IOException
import java.io.OutputStream
import java.util.UUID

/** One file or folder in Files, decrypted on this phone. */
data class DriveEntry(
    val id: String,
    val kind: String, // "file" or "folder"
    val name: String,
    /** The folder it's in (empty: the top level). */
    val parent: String,
    val mimeType: String,
    val size: Long, // plaintext bytes
    val encryptedSize: Long, // what counts toward storage
    val modifiedMs: Long,
    /** Versions of one file share a fileId; the highest [version] is the current one. */
    val fileId: String,
    val version: Int,
    val trashed: Boolean,
    val trashedMs: Long,
    /** Whom the owner shared it with (user ids). Only for the owner's own entries. */
    val sharedWith: List<String> = emptyList(),
    /** Someone else's file, shared with this account (read-only). */
    val incoming: Boolean = false,
    val ownerId: String = "",
    /** Everything that was sealed: the file key and header, link secrets, the lock, ... */
    val meta: JsonObject,
) {
    val isFile: Boolean get() = kind == "file"

    /** Locked under a key or password of the person's own (FileLock.kt). */
    val locked: Boolean get() = FileLocks.isLocked(meta)

    /** Locked, and its key hasn't been unlocked in this copy (see [DriveRepository.unlock]). */
    val needsUnlock: Boolean get() = locked && !meta.containsKey("key")
}

data class DriveSnapshot(
    val entries: List<DriveEntry> = emptyList(), // own files (every version) and folders
    val incoming: List<DriveEntry> = emptyList(), // shared with this account
    val links: List<DriveLinkDto> = emptyList(),
    val usage: DriveUsageDto? = null,
)

enum class DriveStage { Encrypting, Uploading, Downloading, Decrypting }

/**
 * Encrypted cloud storage on this phone: a port of the desktop app's
 * DriveRepository (desktop/core/src/data/drive_repository.cpp). Encrypts
 * files (and their names, folders and types) before upload, decrypts them
 * after download, and keeps every readable detail in metadata sealed under
 * the drive key. Plaintext is only ever written to this app's private cache.
 */
class DriveRepository(
    private val api: DriveApi,
    private val users: ApiService,
    private val drive: DriveCrypto,
    private val vault: VaultCrypto,
    private val files: FileCrypto,
    private val locks: FileLocks,
    private val identity: SelfIdentity,
    httpClient: OkHttpClient,
    apiBase: String,
    private val contentResolver: ContentResolver,
    cacheRoot: File,
    private val contactKeys: ContactKeyPins? = null,
) {
    private val transfer = ProxyTransfer(httpClient, apiBase, "/backup/")

    // Resumable transfers (Tor routing, docs/tor-routing-plan.md): unfinished uploads' sealed journals and
    // encrypted copies, and partial downloads. Null: one-shot transfers, as before.
    private var journalDir: File? = null
    private var journal: SealedRecords? = null
    private val cacheDir = File(cacheRoot, "drive")
    private val json = Json { ignoreUnknownKeys = true }

    /** Where share links open: the web app next to the API (".../redvaultmessages/"). */
    val webAppBase: String = apiBase.removeSuffix("/").removeSuffix("api").let { if (it.endsWith("/")) it else "$it/" }

    private val driveKey: ByteArray get() = drive.driveKey(identity.privateKey)

    /**
     * Over Tor a big file can take hours and a circuit can drop: uploads then go in parts
     * ([PART_SIZE]) that the server checks one by one (SHA-256), with the encrypted copy and a
     * sealed journal kept in [dir] until the server has every part, so an interrupted upload,
     * even across a restart, continues from the first missing part. Downloads continue where
     * they stopped (HTTP Range), and the whole ciphertext is checked before it's decrypted.
     */
    fun enableResumable(dir: File, records: SealedRecords) {
        File(dir, "downloads").mkdirs()
        journalDir = dir
        journal = records
    }

    val resumable: Boolean get() = journalDir != null

    /** Uploads that didn't finish, oldest first. Journals that don't open (another account's) are skipped. */
    fun pendingUploads(): List<PendingUpload> {
        val dir = journalDir ?: return emptyList()
        return (dir.listFiles { f -> f.name.endsWith(".upload") } ?: emptyArray())
            .mapNotNull { readJournal(it.name.removeSuffix(".upload")) }
            .sortedBy { it.createdMs }
            .map { PendingUpload(it.objectId, it.name, it.parent, it.size, it.partCount) }
    }

    /** Sends what the server is missing of a pending upload, then completes it. */
    suspend fun resumeUpload(objectId: String, onProgress: (DriveStage, Long, Long) -> Unit): DriveEntry = mapErrors("Upload failed. Try again.") {
        val record = readJournal(objectId) ?: throw UserFacingException("This upload can't be resumed. Start it again.")
        try {
            sendParts(record, onProgress)
        } catch (e: ApiException) {
            // Gone for good on the server (expired, deleted, refused): nothing left to resume.
            if (e.status == 404 || e.status == 410 || e.status == 400) forgetUpload(objectId)
            throw e
        }
    }

    /** Cancels a pending upload for good: the server drops it, and the local copy and journal go. */
    suspend fun abandonUpload(objectId: String) {
        runCatching { apiCall(ApiClient.json) { api.remove(objectId) } } // unfinished uploads expire on the server anyway
        forgetUpload(objectId)
    }

    private fun forgetUpload(objectId: String) {
        val dir = journalDir ?: return
        File(dir, "$objectId.ciphertext").delete()
        File(dir, "$objectId.upload").delete()
    }

    private fun writeJournal(record: UploadJournal) {
        val dir = journalDir ?: return
        val sealed = journal!!.seal(json.encodeToString(UploadJournal.serializer(), record).toByteArray(Charsets.UTF_8), "drive-upload:${record.objectId}")
        val target = File(dir, "${record.objectId}.upload")
        val temp = File(dir, "${record.objectId}.upload.tmp")
        temp.writeBytes(sealed)
        if (!temp.renameTo(target)) {
            target.delete()
            if (!temp.renameTo(target)) throw UserFacingException("Couldn't save the upload's progress.")
        }
    }

    private fun readJournal(objectId: String): UploadJournal? {
        val dir = journalDir ?: return null
        val file = File(dir, "$objectId.upload")
        if (!file.exists()) return null
        val plain = journal?.open(file.readBytes(), "drive-upload:$objectId") ?: return null
        return runCatching { json.decodeFromString(UploadJournal.serializer(), String(plain, Charsets.UTF_8)) }.getOrNull()
            ?.takeIf { it.objectId == objectId }
    }

    /** The parts path of [upload]: hashes, the server's go-ahead, the journal, then [sendParts]. */
    private suspend fun uploadInParts(
        ciphertext: File,
        encryptedSize: Long,
        ciphertextSha256: String,
        entry: DriveEntry,
        metaCiphertext: String,
        metaNonce: String,
        onStarted: (objectId: String) -> Unit,
        onProgress: (DriveStage, Long, Long) -> Unit,
    ): DriveEntry {
        val dir = journalDir!!
        val partCount = maxOf(1L, (encryptedSize + PART_SIZE - 1) / PART_SIZE).toInt()
        val (hashes, manifest) = withContext(Dispatchers.IO) { Parts.hashes(ciphertext, encryptedSize, PART_SIZE, partCount) }
        val auth = apiCall(ApiClient.json) {
            api.requestUploadParts(DriveUploadPartsRequest(encryptedSize, ciphertextSha256, metaCiphertext, metaNonce, PART_SIZE, manifest))
        }
        if (auth.upload.mode != "parts" || auth.upload.partCount != partCount) throw UserFacingException("Upload failed. Try again.")
        val record = UploadJournal(
            objectId = auth.objectId, name = entry.name, parent = entry.parent, mimeType = entry.mimeType, size = entry.size,
            modifiedMs = entry.modifiedMs, fileId = entry.fileId, version = entry.version, meta = entry.meta,
            encryptedSize = encryptedSize, partSize = PART_SIZE, partCount = partCount, partSha256 = hashes, createdMs = System.currentTimeMillis(),
        )
        // The encrypted copy and the sealed journal outlive this session: a restart resumes them.
        withContext(Dispatchers.IO) {
            ciphertext.copyTo(File(dir, "${auth.objectId}.ciphertext"), overwrite = true)
            writeJournal(record)
        }
        onStarted(auth.objectId)
        return sendParts(record, onProgress)
    }

    /** Sends the parts the server doesn't have (each checked again before it goes), then completes the upload. */
    private suspend fun sendParts(record: UploadJournal, onProgress: (DriveStage, Long, Long) -> Unit): DriveEntry {
        val dir = journalDir!!
        val copy = File(dir, "${record.objectId}.ciphertext")
        if (!copy.exists()) throw UserFacingException("The upload's encrypted copy is missing. Start it again.")
        val have = HashSet<Int>()
        var done = 0L
        for (part in apiCall(ApiClient.json) { api.listParts(record.objectId) }.received) {
            if (part.partNumber in 1..record.partCount && part.sha256 == record.partSha256[part.partNumber - 1]) {
                have += part.partNumber
                done += part.size
            }
        }
        onProgress(DriveStage.Uploading, done, record.encryptedSize)
        for (n in 1..record.partCount) {
            if (n in have) continue
            val bytes = withContext(Dispatchers.IO) { Parts.read(copy, record.encryptedSize, record.partSize, record.partCount, n) }
            val hash = record.partSha256[n - 1]
            // A copy changed on disk must not go up as this part.
            if (Parts.sha256Hex(bytes) != hash) throw UserFacingException("The upload's encrypted copy is damaged. Start it again.")
            val target = TransferTargetDto(
                mode = "proxy", url = "/backup/${record.objectId}/parts/$n", method = "PUT",
                headers = mapOf("Content-Type" to "application/octet-stream", "X-Content-Sha256" to hash), expiresAt = "",
            )
            val before = done
            transfer.run(target) { client, url, headers ->
                val body = CountingRequestBody(bytes.toRequestBody("application/octet-stream".toMediaType())) { sent ->
                    onProgress(DriveStage.Uploading, before + sent, record.encryptedSize)
                }
                client.newCall(Request.Builder().url(url).put(body).headers(headers).build()).await()
            }.close()
            done += bytes.size
        }
        retryTransient { apiCall(ApiClient.json) { api.completeUpload(record.objectId, JsonObject(emptyMap())) } }
        forgetUpload(record.objectId)
        return DriveEntry(
            id = record.objectId, kind = "file", name = record.name, parent = record.parent, mimeType = record.mimeType,
            size = record.size, encryptedSize = record.encryptedSize, modifiedMs = record.modifiedMs, fileId = record.fileId,
            version = record.version, trashed = false, trashedMs = 0, meta = record.meta,
        ).let { if (it.locked) it.copy(meta = JsonObject(it.meta - "key")) else it }
    }

    suspend fun refresh(): DriveSnapshot = mapErrors("Couldn't load your files. Try again.") {
        val listing = apiCall(ApiClient.json) { api.list() }
        val entries = listing.items.mapNotNull { runCatching { decode(it) }.getOrNull() }
        val incoming = apiCall(ApiClient.json) { api.sharedWithMe() }.items.mapNotNull { s ->
            runCatching {
                val meta = parse(vault.openFromSender(Base64Codec.decode(s.sealedMeta), identity.publicKey, identity.privateKey))
                entryFrom(meta, s.id, "file", s.encryptedSize).copy(incoming = true, ownerId = s.ownerId, parent = "")
            }.getOrNull()
        }
        val links = apiCall(ApiClient.json) { api.links() }.links
        DriveSnapshot(entries, incoming, links, listing.usage)
    }

    suspend fun createFolder(name: String, parent: String): DriveEntry = mapErrors("Couldn't create the folder. Try again.") {
        val folder = DriveEntry(
            id = "", kind = "folder", name = FileMessage.sanitizeFilename(name.ifBlank { "New folder" }), parent = parent,
            mimeType = "", size = 0, encryptedSize = 0, modifiedMs = System.currentTimeMillis(), fileId = "", version = 1,
            trashed = false, trashedMs = 0, meta = JsonObject(emptyMap()),
        )
        val sealed = drive.sealMeta(sealedMetaJson(folder), driveKey)
        decode(apiCall(ApiClient.json) { api.createFolder(DriveMetaRequest(Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce))) })
    }

    /**
     * Encrypts and uploads a picked file into [parent]. With [replacing], it
     * becomes that file's next version. With [lock], the file's key is also
     * locked under that key or password. Returns the entry locked (its key
     * isn't kept).
     */
    suspend fun upload(
        uri: Uri,
        parent: String,
        replacing: DriveEntry?,
        lock: FileLock?,
        onProgress: (DriveStage, Long, Long) -> Unit,
        /** Resumable uploads: once the server knows it (its id), before the first part goes. */
        onStarted: (objectId: String) -> Unit = {},
    ): DriveEntry = mapErrors("Upload failed. Try again.") {
        if (lock is FileLock.WithPassword) locks.passwordProblem(lock.password)?.let { throw UserFacingException(it) }
        val (name, sizeHint) = withContext(Dispatchers.IO) { describe(uri) }
        val plainSize = sizeHint ?: 0L
        val encryptedEstimate = encryptedSizeFor(plainSize)

        // Say so right away, before spending minutes encrypting something that can't fit.
        val usage = apiCall(ApiClient.json) { api.usage() }
        if (sizeHint != null && usage.used + encryptedEstimate > usage.limit) {
            val upgrade = when (usage.tier) {
                "premium" -> ""
                "pro" -> ", or upgrade to Premium for more space"
                else -> ", or upgrade to Pro or Premium for more space"
            }
            throw UserFacingException(
                "Not enough storage: this file needs ${formatBytes(encryptedEstimate)} and you have ${formatBytes(usage.remaining)} left of your " +
                    "${formatBytes(usage.limit)} (${planName(usage.tier)}). Delete some files to free up space$upgrade.",
            )
        }

        val outgoing = File(cacheDir, "outgoing").apply { mkdirs() }
        val ciphertext = File(outgoing, UUID.randomUUID().toString())
        try {
            val encrypted = withContext(Dispatchers.IO) {
                val input = contentResolver.openInputStream(uri) ?: throw UserFacingException("Couldn't read that file.")
                input.use { files.encryptFile(it, ciphertext) { done -> onProgress(DriveStage.Encrypting, done, plainSize) } }
            }
            val fileId = replacing?.fileId ?: UUID.randomUUID().toString()
            val meta = buildJsonObject {
                put("key", JsonPrimitive(Base64Codec.encode(encrypted.key)))
                put("header", JsonPrimitive(Base64Codec.encode(encrypted.header)))
                put("chunk", JsonPrimitive(FileCrypto.CHUNK_BYTES))
                put("sha", JsonPrimitive(encrypted.ciphertextSha256))
                if (lock != null) put("lock", locks.lock(encrypted.key, lock, fileId))
            }
            encrypted.key.fill(0)
            val entry = DriveEntry(
                id = "",
                kind = "file",
                name = replacing?.name ?: FileMessage.sanitizeFilename(name ?: "file"),
                parent = replacing?.parent ?: parent,
                mimeType = contentResolver.getType(uri) ?: guessMime(name ?: ""),
                size = encrypted.plaintextSize,
                encryptedSize = encrypted.encryptedSize,
                modifiedMs = System.currentTimeMillis(),
                fileId = fileId,
                version = (replacing?.version ?: 0) + 1,
                trashed = false,
                trashedMs = 0,
                meta = meta,
            )
            val sealed = drive.sealMeta(sealedMetaJson(entry), driveKey)
            if (resumable) {
                return@mapErrors uploadInParts(
                    ciphertext, encrypted.encryptedSize, encrypted.ciphertextSha256, entry,
                    Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce), onStarted, onProgress,
                )
            }
            val auth = apiCall(ApiClient.json) {
                api.requestUpload(
                    DriveUploadRequest(encrypted.encryptedSize, encrypted.ciphertextSha256, Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce)),
                )
            }
            if (auth.upload.method != "PUT") throw UserFacingException("Upload failed. Try again.")
            withContext(Dispatchers.IO) {
                transfer.run(auth.upload) { client, url, headers ->
                    val body = CountingRequestBody(ciphertext.asRequestBody("application/octet-stream".toMediaType())) { done ->
                        onProgress(DriveStage.Uploading, done, encrypted.encryptedSize)
                    }
                    client.newCall(Request.Builder().url(url).put(body).headers(headers).build()).execute()
                }.close()
            }
            retryTransient { apiCall(ApiClient.json) { api.completeUpload(auth.objectId, JsonObject(emptyMap())) } }
            // Uploaded locked: it stays locked until it's unlocked on purpose.
            val stored = entry.copy(id = auth.objectId)
            if (stored.locked) stored.copy(meta = JsonObject(stored.meta - "key")) else stored
        } finally {
            ciphertext.delete()
        }
    }

    /** Downloads and decrypts into [output]. A locked entry must be unlocked first. */
    suspend fun download(entry: DriveEntry, output: OutputStream, onProgress: (DriveStage, Long, Long) -> Unit = { _, _, _ -> }) {
        requireUnlocked(entry)
        mapErrors("Download failed. Try again.") {
            val target = apiCall(ApiClient.json) { api.download(entry.id) }.download
            if (target.method != "GET") throw UserFacingException("Download failed. Try again.")
            val dir = journalDir
            if (dir != null) return@mapErrors downloadResumable(entry, target, File(dir, "downloads/${entry.id}.ciphertext"), output, onProgress)
            withContext(Dispatchers.IO) {
                transfer.run(target) { client, url, headers -> client.newCall(Request.Builder().url(url).get().headers(headers).build()).execute() }
                    .use { response ->
                        val body = response.body ?: throw IOException("Empty response")
                        onProgress(DriveStage.Decrypting, 0, entry.encryptedSize)
                        files.decrypt(
                            input = body.byteStream(),
                            output = output,
                            key = Base64Codec.decode(entry.meta.stringOr("key", "")),
                            header = Base64Codec.decode(entry.meta.stringOr("header", "")),
                            encryptedSize = entry.encryptedSize,
                            ciphertextSha256 = entry.meta.stringOr("sha", ""),
                        ) { done -> onProgress(DriveStage.Downloading, done, entry.encryptedSize) }
                    }
            }
        }
    }

    /**
     * Fetches the ciphertext into [partial], continuing from what an earlier, interrupted try left
     * there (HTTP Range), then checks it (SHA-256, inside decrypt) and decrypts it into [output].
     * The partial copy is ciphertext only; it goes once decrypted, or if it turns out wrong.
     */
    private suspend fun downloadResumable(
        entry: DriveEntry,
        target: TransferTargetDto,
        partial: File,
        output: OutputStream,
        onProgress: (DriveStage, Long, Long) -> Unit,
    ) {
        val total = entry.encryptedSize
        if (partial.length() > total) partial.delete()
        val have = partial.length()
        if (have < total) {
            val ranged = target.copy(headers = target.headers + ("Range" to "bytes=$have-"))
            transfer.run(ranged) { client, url, headers -> client.newCall(Request.Builder().url(url).get().headers(headers).build()).await() }
                .use { response ->
                    val body = response.body ?: throw IOException("Empty response")
                    // 206: the rest, after what's here. 200: the server sent it all again from the start.
                    val append = response.code == 206
                    withContext(Dispatchers.IO) {
                        java.io.FileOutputStream(partial, append).buffered().use { out ->
                            var written = if (append) have else 0L
                            val buffer = ByteArray(1 shl 16)
                            body.byteStream().use { input ->
                                while (true) {
                                    val read = input.read(buffer)
                                    if (read < 0) break
                                    out.write(buffer, 0, read)
                                    written += read
                                    onProgress(DriveStage.Downloading, written, total)
                                }
                            }
                        }
                    }
                }
        }
        onProgress(DriveStage.Decrypting, total, total)
        try {
            withContext(Dispatchers.IO) {
                partial.inputStream().buffered().use { input ->
                    files.decrypt(
                        input = input,
                        output = output,
                        key = Base64Codec.decode(entry.meta.stringOr("key", "")),
                        header = Base64Codec.decode(entry.meta.stringOr("header", "")),
                        encryptedSize = entry.encryptedSize,
                        ciphertextSha256 = entry.meta.stringOr("sha", ""),
                    )
                }
            }
        } finally {
            // Decrypted, or wrong (a bad copy would fail the same way next time): start over then.
            partial.delete()
        }
    }

    /** Downloads and decrypts into this session's cache (to open or preview); returns the file. */
    suspend fun downloadToCache(entry: DriveEntry, onProgress: (DriveStage, Long, Long) -> Unit = { _, _, _ -> }): File {
        val folder = File(cacheDir, "open/${UUID.randomUUID()}").apply { mkdirs() }
        val file = File(folder, FileMessage.sanitizeFilename(entry.name))
        try {
            file.outputStream().buffered().use { download(entry, it, onProgress) }
        } catch (e: Throwable) {
            folder.deleteRecursively()
            throw e
        }
        return file
    }

    /** Rename, move, trash or restore: the entry's name, parent and trashed state are re-sealed as they are. */
    suspend fun save(entry: DriveEntry): DriveEntry = mapErrors("Couldn't save the change. Try again.") {
        val changed = entry.copy(name = FileMessage.sanitizeFilename(entry.name.ifBlank { "Untitled" }))
        val sealed = drive.sealMeta(sealedMetaJson(changed), driveKey)
        decode(apiCall(ApiClient.json) { api.updateMeta(entry.id, DriveMetaRequest(Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce))) })
            .copy(sharedWith = entry.sharedWith)
    }

    /** Deletes for good, freeing the space (the server removes it from storage too). */
    suspend fun remove(entry: DriveEntry) = mapErrors("Couldn't delete it. Try again.") { apiCall(ApiClient.json) { api.remove(entry.id) } }

    /** Shares with another account: the metadata sealed to their public key. A locked entry must be unlocked first. */
    suspend fun share(entry: DriveEntry, recipientId: String) {
        requireUnlocked(entry)
        mapErrors("Couldn't share it. Try again.") {
            val recipient = apiCall(ApiClient.json) { users.getUser(recipientId) }
            if (contactKeys?.keyChanged(identity.userId, recipientId, recipient.publicKey, verified = false) == true) {
                throw UserFacingException(KeyTrust.CONTACT_KEY_CHANGED)
            }
            val sealed = vault.sealForRecipient(recipientMetaJson(entry), Base64Codec.decode(recipient.publicKey))
            apiCall(ApiClient.json) { api.share(entry.id, DriveShareRequest(recipientId, Base64Codec.encode(sealed))) }
        }
    }

    suspend fun unshare(entry: DriveEntry, recipientId: String) =
        mapErrors("Couldn't stop sharing it. Try again.") { apiCall(ApiClient.json) { api.unshare(entry.id, recipientId) } }

    data class LinkOptions(val password: String = "", val expiresInDays: Int? = 7, val allowDownload: Boolean = false)

    /** A share link; returns its URL and the entry (the secret is kept in its metadata so the link can be copied again). */
    suspend fun createLink(entry: DriveEntry, options: LinkOptions): Pair<String, DriveEntry> {
        requireUnlocked(entry)
        return mapErrors("Couldn't create the link. Try again.") {
            val secret = drive.newLinkSecret()
            val salt = if (options.password.isEmpty()) null else drive.newPasswordSalt()
            val key = drive.linkKey(secret, options.password, salt ?: ByteArray(0))
            // What the viewer needs to decrypt and name the file: nothing about folders or other files.
            val linkMeta = buildJsonObject {
                put("v", JsonPrimitive(1))
                put("name", JsonPrimitive(entry.name))
                put("mime", JsonPrimitive(entry.mimeType))
                put("size", JsonPrimitive(entry.size))
                put("key", entry.meta["key"]!!)
                put("header", entry.meta["header"]!!)
                put("chunk", entry.meta["chunk"] ?: JsonPrimitive(FileCrypto.CHUNK_BYTES))
                put("sha", entry.meta["sha"]!!)
            }
            val sealed = drive.sealMeta(linkMeta.toString(), key)
            key.fill(0)
            val link = apiCall(ApiClient.json) {
                api.createLink(
                    entry.id,
                    DriveLinkRequest(
                        Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce), salt?.let(Base64Codec::encode),
                        options.allowDownload, options.expiresInDays,
                    ),
                )
            }
            val links = (entry.meta["links"] as? JsonObject ?: JsonObject(emptyMap())) + (link.id to JsonPrimitive(drive.linkSecretToFragment(secret)))
            val saved = save(entry.copy(meta = JsonObject(entry.meta + ("links" to JsonObject(links)))))
            drive.fileLinkUrl(webAppBase, link.id, secret) to saved
        }
    }

    suspend fun deleteLink(entry: DriveEntry, linkId: String): DriveEntry = mapErrors("Couldn't remove the link. Try again.") {
        apiCall(ApiClient.json) { api.deleteLink(linkId) }
        val links = entry.meta["links"] as? JsonObject
        if (links != null && links.containsKey(linkId)) save(entry.copy(meta = JsonObject(entry.meta + ("links" to JsonObject(links - linkId))))) else entry
    }

    /** The URL of an existing link of this file, or null (a link made on another device keeps its secret there). */
    fun linkUrl(entry: DriveEntry, linkId: String): String? {
        val fragment = (entry.meta["links"] as? JsonObject)?.stringOr(linkId, "")?.takeIf { it.isNotEmpty() } ?: return null
        val secret = drive.linkSecretFromFragment(fragment) ?: return null
        return drive.fileLinkUrl(webAppBase, linkId, secret)
    }

    /**
     * A copy of a locked entry with its file key unlocked, in memory only (it's
     * never saved that way). Throws [UserFacingException] for a wrong key or
     * password. Slow for a password (Argon2id).
     */
    suspend fun unlock(entry: DriveEntry, with: FileLock): DriveEntry = withContext(Dispatchers.Default) {
        if (!entry.locked) return@withContext entry
        val key = locks.unlock(entry.meta["lock"]!!.jsonObject, with, entry.fileId)
        try {
            entry.copy(meta = JsonObject(entry.meta + ("key" to JsonPrimitive(Base64Codec.encode(key)))))
        } finally {
            key.fill(0)
        }
    }

    /**
     * Locks an entry's key under [lock], changes its lock, or (null) removes it.
     * The entry must be unlocked first. Only this version changes; the file
     * isn't re-encrypted or re-uploaded. Returns it still unlocked.
     */
    suspend fun setLock(entry: DriveEntry, lock: FileLock?): DriveEntry {
        requireUnlocked(entry)
        if (!entry.isFile || entry.incoming) throw UserFacingException("Only your own files can be locked.")
        if (lock is FileLock.WithPassword) locks.passwordProblem(lock.password)?.let { throw UserFacingException(it) }
        val key = entry.meta["key"]!!
        val newMeta = if (lock == null) {
            JsonObject(entry.meta - "lock")
        } else {
            val fileKey = Base64Codec.decode((key as JsonPrimitive).content)
            val lockJson = try {
                mapErrors("Couldn't lock the file. Try again.") { withContext(Dispatchers.Default) { locks.lock(fileKey, lock, entry.fileId) } }
            } finally {
                fileKey.fill(0)
            }
            JsonObject(entry.meta + ("lock" to lockJson))
        }
        val saved = save(entry.copy(meta = newMeta))
        return saved.copy(meta = JsonObject(saved.meta + ("key" to key)))
    }

    /** Removes every decrypted file and leftover upload. */
    fun clearCache() {
        cacheDir.deleteRecursively()
    }

    // --- Metadata ---

    private fun decode(dto: DriveEntryDto): DriveEntry {
        val meta = parse(drive.openMeta(Base64Codec.decode(dto.metaCiphertext), Base64Codec.decode(dto.metaNonce), driveKey))
        return entryFrom(meta, dto.id, dto.kind, dto.encryptedSize).copy(sharedWith = dto.sharedWith)
    }

    private fun parse(text: String): JsonObject = json.parseToJsonElement(text).jsonObject

    private fun entryFrom(meta: JsonObject, id: String, kind: String, encryptedSize: Long) = DriveEntry(
        id = id,
        kind = meta.stringOr("kind", kind),
        name = meta.stringOr("name", "Untitled"),
        parent = meta.stringOr("parent", ""),
        mimeType = meta.stringOr("mime", "application/octet-stream"),
        size = meta.longOr("size", 0),
        encryptedSize = encryptedSize,
        modifiedMs = meta.longOr("modified", 0),
        fileId = meta.stringOr("fileId", id),
        version = meta.intOr("version", 1),
        trashed = meta.boolOr("trashed", false),
        trashedMs = meta.longOr("trashedAt", 0),
        meta = meta,
    )

    private fun sealedMetaJson(entry: DriveEntry): String {
        val out = entry.meta.toMutableMap<String, JsonElement>()
        out["v"] = JsonPrimitive(1)
        out["kind"] = JsonPrimitive(entry.kind)
        out["name"] = JsonPrimitive(entry.name)
        out["parent"] = JsonPrimitive(entry.parent)
        out["modified"] = JsonPrimitive(entry.modifiedMs)
        out["trashed"] = JsonPrimitive(entry.trashed)
        out["trashedAt"] = JsonPrimitive(entry.trashedMs)
        if (entry.isFile) {
            out["mime"] = JsonPrimitive(entry.mimeType)
            out["size"] = JsonPrimitive(entry.size)
            out["fileId"] = JsonPrimitive(entry.fileId)
            out["version"] = JsonPrimitive(entry.version)
        }
        // A locked file's key is only ever stored locked: an unlocked copy's key never goes back to the server.
        if (FileLocks.isLocked(JsonObject(out))) out.remove("key")
        return JsonObject(out).toString()
    }

    /** The metadata as someone it's shared with gets it: the file key in the clear (inside their sealed copy), no lock. */
    private fun recipientMetaJson(entry: DriveEntry): String {
        val out = parse(sealedMetaJson(entry)).toMutableMap()
        out.remove("lock")
        entry.meta["key"]?.let { out["key"] = it }
        return JsonObject(out).toString()
    }

    private fun requireUnlocked(entry: DriveEntry) {
        if (entry.needsUnlock) throw UserFacingException("This file is locked. Unlock it with its key or password first.")
    }

    // --- Helpers ---

    private fun describe(uri: Uri): Pair<String?, Long?> {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val name = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME).takeIf { it >= 0 }?.let(cursor::getString)
                val size = cursor.getColumnIndex(OpenableColumns.SIZE).takeIf { it >= 0 && !cursor.isNull(it) }?.let(cursor::getLong)
                return (name ?: uri.lastPathSegment) to size
            }
        }
        // A file:// Uri (shared from another app, or a test): its own name, and its size on disk.
        return uri.lastPathSegment to uri.path?.takeIf { uri.scheme == "file" }?.let { java.io.File(it).length() }
    }

    private suspend fun <T> retryTransient(block: suspend () -> T): T {
        var attempt = 0
        while (true) {
            try {
                return block()
            } catch (e: ApiException) {
                val transient = e.code == "network_error" || e.status >= 500 || e.code == "upload_incomplete"
                if (!transient || ++attempt >= 4) throw e
                delay(400L shl (attempt - 1))
            }
        }
    }

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
                    e.code in SERVER_WORDED -> e.message ?: fallback
                    e.code == "backup_disabled" -> "Files aren't available on this server."
                    e.code == "tor_unavailable" -> throw TransferInterruptedException(TOR_UNAVAILABLE_MESSAGE)
                    e.code == "network_error" || e.status >= 500 -> throw TransferInterruptedException()
                    e.status == 404 -> "That file isn't there anymore."
                    e.status == 410 -> "This file is no longer available."
                    else -> fallback
                },
            )
        } catch (e: CryptoException) {
            throw UserFacingException(fallback)
        } catch (e: TorUnavailableException) {
            throw TransferInterruptedException(TOR_UNAVAILABLE_MESSAGE)
        } catch (e: IOException) {
            throw TransferInterruptedException()
        } catch (e: kotlinx.serialization.SerializationException) {
            throw UserFacingException(fallback)
        } catch (e: IllegalArgumentException) {
            throw UserFacingException(fallback)
        }
    }

    companion object {
        /** Resumable uploads' part size: the desktop app's too (kPartSize). */
        const val PART_SIZE = 8L * 1024 * 1024

        private val SERVER_WORDED = setOf(
            "backup_storage_full", "backup_too_large", "too_many_pending_uploads", "rate_limited", "backup_deleted", "backup_missing", "upload_expired",
        )

        /** Ciphertext size for a plaintext size: 17 bytes of overhead per 64 KiB chunk (at least one chunk). */
        fun encryptedSizeFor(plainSize: Long): Long {
            val chunks = maxOf(1L, (plainSize + FileCrypto.CHUNK_BYTES - 1) / FileCrypto.CHUNK_BYTES)
            return plainSize + chunks * FileCrypto.CHUNK_OVERHEAD_BYTES
        }

        fun planName(tier: String) = when (tier) {
            "premium" -> "Premium"
            "pro" -> "Pro"
            else -> "Free"
        }

        /** "1.5 GB": the server's formatBytes, so both word storage the same way. */
        fun formatBytes(bytes: Long): String {
            val units = listOf("bytes", "KB", "MB", "GB", "TB")
            var value = bytes.toDouble()
            var unit = 0
            while (value >= 1024 && unit < 4) {
                value /= 1024
                unit++
            }
            return if (unit == 0 || value >= 10) "%.0f %s".format(value, units[unit]) else "%.1f %s".format(value, units[unit])
        }

        fun guessMime(name: String): String =
            MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substringAfterLast('.', "").lowercase()) ?: "application/octet-stream"
    }
}
