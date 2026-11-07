package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.data.api.TOR_UNAVAILABLE_MESSAGE
import com.redvaultsystems.redvaultmessage.tor.TorUnavailableException
import android.content.ContentResolver
import android.net.Uri
import android.provider.OpenableColumns
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileDecryptException
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.FileConfigDto
import com.redvaultsystems.redvaultmessage.data.api.FileStorageUsageDto
import com.redvaultsystems.redvaultmessage.data.api.FileUploadRequest
import com.redvaultsystems.redvaultmessage.data.api.TransferTargetDto
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.model.ConversationMeta
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileEncryptionInfo
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import com.redvaultsystems.redvaultmessage.data.model.Message
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonObject
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.asRequestBody
import okhttp3.Response
import java.io.File
import java.io.IOException
import java.time.Instant
import java.util.UUID

enum class FileSendStage { Encrypting, Uploading, Sending }

enum class FileDownloadStage { Downloading, Decrypting }

/**
 * Port of sendDmFile/downloadDmFile (ui/src/state/actions.ts) and
 * ui/src/lib/fileTransfer.ts. Only ciphertext ever leaves the phone, and
 * plaintext is only ever written to this app's private cache, which is
 * cleared when the session ends.
 *
 * Only "proxy" transfer targets are followed: a path on our own API, so the
 * storage provider never sees the user's IP address or when they transfer
 * anything. A "direct" target (a presigned URL at the storage provider) is
 * refused outright; the web app's CSP refuses it the same way.
 */
class FileRepository(
    private val api: ApiService,
    private val conversations: ConversationRepository,
    private val fileCrypto: FileCrypto,
    httpClient: OkHttpClient,
    private val apiBase: String,
    private val contentResolver: ContentResolver,
    cacheDir: File,
) {
    private val proxyClient = httpClient
    private val outgoingDir = File(cacheDir, "outgoing")
    private val decryptedDir = File(cacheDir, "decrypted")
    private var config: FileConfigDto? = null

    /** Encrypts, uploads and sends a picked file. Returns only once the message itself is stored. */
    suspend fun send(meta: ConversationMeta, uri: Uri, onStage: (FileSendStage) -> Unit): Message {
        val (name, size) = withContext(Dispatchers.IO) { describe(uri) }
        return sendStream(
            meta,
            name = name ?: "file",
            sizeHint = size,
            mimeType = contentResolver.getType(uri) ?: "application/octet-stream",
            open = { contentResolver.openInputStream(uri) ?: throw UserFacingException("Couldn't read that file.") },
            onStage = onStage,
        )
    }

    /** Like [send], for a file read from [picked] (a picture pasted into the message box, kept in memory until now). */
    suspend fun sendPicked(meta: ConversationMeta, picked: PickedFile, onStage: (FileSendStage) -> Unit): Message =
        sendStream(meta, picked.name, picked.size, picked.mimeType, picked.open, onStage)

    /** The server's file size limit, once it's been fetched (null before the first upload). */
    val maxFileSizeIfKnown: Long? get() = config?.maxFileSize

    /** A picked file's name, type and size, and a way to read it (for the Tor outbox). */
    suspend fun picked(uri: Uri): PickedFile = withContext(Dispatchers.IO) {
        val (name, size) = describe(uri)
        PickedFile(name ?: "file", contentResolver.getType(uri) ?: "application/octet-stream", size) {
            contentResolver.openInputStream(uri) ?: throw UserFacingException("Couldn't read that file.")
        }
    }

    /**
     * Sends a file queued in the Tor outbox: [decryptInto] writes its plaintext into a private
     * temporary file, deleted right after. [clientMessageId] (the outbox item's id) makes the
     * final send idempotent: sent twice after a lost answer, it's still stored once.
     */
    suspend fun sendFromOutbox(
        meta: ConversationMeta,
        name: String,
        mimeType: String,
        clientMessageId: String,
        decryptInto: (File) -> Unit,
    ): Message {
        outgoingDir.mkdirs()
        val plain = File(outgoingDir, "queued-" + UUID.randomUUID())
        try {
            withContext(Dispatchers.IO) { decryptInto(plain) }
            return sendStream(meta, name, plain.length(), mimeType, { plain.inputStream() }, {}, clientMessageId)
        } finally {
            plain.delete()
        }
    }

    /** A voice message recorded by this app (see VoiceRecorder): same encryption and upload as any file. */
    suspend fun sendVoice(meta: ConversationMeta, recording: File, onStage: (FileSendStage) -> Unit): Message =
        sendStream(meta, VOICE_FILENAME, recording.length(), VOICE_MIME_TYPE, { recording.inputStream() }, onStage)

    /**
     * Forwards a received file to another chat: decrypts it into the cache,
     * then sends it there as a new file with a new key, so the two chats
     * never share an object or a key. The decrypted copy is deleted after.
     */
    suspend fun forward(meta: ConversationMeta, descriptor: FileDescriptor, onStage: (FileSendStage) -> Unit): Message {
        val plain = download(descriptor) {}
        try {
            return sendStream(meta, descriptor.filename, plain.length(), descriptor.mimeType, { plain.inputStream() }, onStage)
        } finally {
            plain.delete()
            plain.parentFile?.delete()
        }
    }

    /**
     * [forward] to several chats: decrypted once, then sent to each as its own new file and key.
     * [onSent] gets each message as it's stored; a failure stops there (the ones before stay sent).
     */
    suspend fun forwardToMany(metas: List<ConversationMeta>, descriptor: FileDescriptor, onSent: (ConversationMeta, Message) -> Unit) {
        if (metas.isEmpty()) return
        val plain = download(descriptor) {}
        try {
            for (meta in metas) {
                onSent(meta, sendStream(meta, descriptor.filename, plain.length(), descriptor.mimeType, { plain.inputStream() }, {}))
            }
        } finally {
            plain.delete()
            plain.parentFile?.delete()
        }
    }

    private suspend fun sendStream(
        meta: ConversationMeta,
        name: String,
        sizeHint: Long?,
        mimeType: String,
        open: () -> java.io.InputStream,
        onStage: (FileSendStage) -> Unit,
        clientMessageId: String? = null,
    ): Message = mapErrors("Upload failed. Try again.") {
        val descriptor = encryptAndUpload(meta.id, name, sizeHint, mimeType, open, onStage, release = false)
        retryTransient { conversations.sendFile(meta, descriptor, clientMessageId) }
    }

    /**
     * The upload half of a file message, for a chat on device keys (uploadFileForDeviceKeys in actions.ts): encrypts,
     * uploads and releases it, because no server message will ever attach it. The descriptor, with the file's key,
     * then goes in the encrypted envelope; the server only learns that a file became available in this conversation.
     */
    suspend fun uploadForDeviceKeys(conversationId: String, picked: PickedFile, onStage: (FileSendStage) -> Unit): FileDescriptor =
        mapErrors("Upload failed. Try again.") {
            encryptAndUpload(conversationId, picked.name, picked.size, picked.mimeType, picked.open, onStage, release = true)
        }

    /** Removes a file from the server (uploader only), for "delete for everyone" on device keys. */
    suspend fun deleteFile(objectId: String) {
        apiCall(ApiClient.json) { api.deleteFile(objectId) }
    }

    private suspend fun encryptAndUpload(
        conversationId: String,
        name: String,
        sizeHint: Long?,
        mimeType: String,
        open: () -> java.io.InputStream,
        onStage: (FileSendStage) -> Unit,
        release: Boolean,
    ): FileDescriptor {
        val limits = config ?: apiCall(ApiClient.json) { api.fileConfig() }.also { config = it }
        val size = sizeHint
        if (size != null && size > limits.maxFileSize) {
            throw UserFacingException("File too large. The limit is ${limits.maxFileSize / (1024 * 1024)} MB.")
        }

        onStage(FileSendStage.Encrypting)
        outgoingDir.mkdirs()
        val ciphertext = File(outgoingDir, UUID.randomUUID().toString())
        try {
            val encrypted = withContext(Dispatchers.IO) { open().use { fileCrypto.encryptFile(it, ciphertext) } }
            if (encrypted.plaintextSize > limits.maxFileSize) {
                throw UserFacingException("File too large. The limit is ${limits.maxFileSize / (1024 * 1024)} MB.")
            }

            onStage(FileSendStage.Uploading)
            val auth = retryTransient {
                apiCall(ApiClient.json) {
                    api.requestFileUpload(FileUploadRequest(conversationId, encrypted.encryptedSize, encrypted.ciphertextSha256))
                }
            }
            upload(auth.upload, ciphertext)
            retryTransient { apiCall(ApiClient.json) { api.completeFileUpload(auth.objectId, JsonObject(emptyMap())) } }
            if (release) retryTransient { apiCall(ApiClient.json) { api.releaseFile(auth.objectId, JsonObject(emptyMap())) } }

            onStage(FileSendStage.Sending)
            val descriptor = FileDescriptor(
                type = "file",
                version = FileCrypto.FORMAT_VERSION,
                objectId = auth.objectId.lowercase(),
                fileKey = Base64Codec.encode(encrypted.key),
                filename = FileMessage.sanitizeFilename(name),
                // Informational only, never a security decision.
                mimeType = mimeType,
                originalSize = encrypted.plaintextSize,
                encryptedSize = encrypted.encryptedSize,
                ciphertextSha256 = encrypted.ciphertextSha256,
                encryption = FileEncryptionInfo(
                    FileCrypto.ALGORITHM, FileCrypto.FORMAT_VERSION, Base64Codec.encode(encrypted.header), FileCrypto.CHUNK_BYTES,
                ),
            )
            encrypted.key.fill(0)
            return descriptor
        } finally {
            ciphertext.delete()
        }
    }

    /**
     * Fetches a received file's ciphertext, verifies it and decrypts it into
     * this app's private cache. Returns the plaintext file; nothing
     * decrypted is ever sent anywhere.
     */
    suspend fun download(descriptor: FileDescriptor, onStage: (FileDownloadStage) -> Unit): File = mapErrors("Download failed. Try again.") {
        onStage(FileDownloadStage.Downloading)
        val target = retryTransient { apiCall(ApiClient.json) { api.fileDownload(descriptor.objectId) } }.download
        if (target.method != "GET") throw UserFacingException("Download failed. Try again.")

        // One folder per download, so two files with the same name never collide.
        val folder = File(decryptedDir, UUID.randomUUID().toString()).apply { mkdirs() }
        val output = File(folder, descriptor.filename)
        try {
            withContext(Dispatchers.IO) {
                transfer(target) { client, url ->
                    client.newCall(Request.Builder().url(url).get().headers(headersOf(target)).build()).execute()
                }.use { response ->
                    onStage(FileDownloadStage.Decrypting)
                    val body = response.body ?: throw IOException("Empty response")
                    output.outputStream().buffered().use { out ->
                        fileCrypto.decrypt(
                            input = body.byteStream(),
                            output = out,
                            key = Base64Codec.decode(descriptor.fileKey),
                            header = Base64Codec.decode(descriptor.encryption.header),
                            encryptedSize = descriptor.encryptedSize,
                            ciphertextSha256 = descriptor.ciphertextSha256,
                        )
                    }
                }
            }
            output
        } catch (e: Throwable) {
            // Never leave partially decrypted output behind.
            folder.deleteRecursively()
            throw e
        }
    }

    /** Plaintext already held in memory (a picture shown in a chat), written to the private cache for Open. */
    suspend fun writeDecrypted(filename: String, bytes: ByteArray): File = withContext(Dispatchers.IO) {
        val folder = File(decryptedDir, UUID.randomUUID().toString()).apply { mkdirs() }
        File(folder, FileMessage.sanitizeFilename(filename)).apply { writeBytes(bytes) }
    }

    /** Your own file storage in use vs. your plan's limit. */
    suspend fun storageUsage(): FileStorageUsageDto = apiCall(ApiClient.json) { api.fileStorageUsage() }

    /** Removes every decrypted file and leftover upload. Called when the session ends. */
    fun clearCache() {
        decryptedDir.deleteRecursively()
        outgoingDir.deleteRecursively()
    }

    private suspend fun upload(target: TransferTargetDto, ciphertext: File) = withContext(Dispatchers.IO) {
        if (target.method != "PUT") throw UserFacingException("Upload failed. Try again.")
        transfer(target) { client, url ->
            val body = ciphertext.asRequestBody("application/octet-stream".toMediaType())
            client.newCall(Request.Builder().url(url).put(body).headers(headersOf(target)).build()).execute()
        }.close()
    }

    /** Runs one transfer with retries on network errors, 5xx and 429; returns the successful response, still open. */
    private suspend fun transfer(target: TransferTargetDto, call: (OkHttpClient, String) -> Response): Response {
        // Never a third-party origin: it would see this phone's IP address.
        if (target.mode != "proxy" || !target.url.startsWith("/v1/files/")) throw UserFacingException("Transfer failed. Try again.")
        val client = proxyClient
        val url = apiBase + target.url.removePrefix("/")
        var lastError: Exception = TransferInterruptedException()
        for (attempt in 0 until MAX_ATTEMPTS) {
            if (attempt > 0) delay(minOf(8_000L, 500L shl (attempt - 1)))
            if (runCatching { Instant.parse(target.expiresAt).toEpochMilli() < System.currentTimeMillis() }.getOrDefault(false)) {
                throw UserFacingException("The transfer link expired. Try again.")
            }
            val response = try {
                call(client, url)
            } catch (e: TorUnavailableException) {
                // Tor routing and Tor is down: retrying now can't help, and nothing may go another way.
                throw TransferInterruptedException(TOR_UNAVAILABLE_MESSAGE)
            } catch (e: IOException) {
                lastError = TransferInterruptedException()
                continue
            }
            if (response.isSuccessful) return response
            val code = response.code
            response.close()
            val error = when {
                code == 410 -> UserFacingException("This file is no longer available.")
                code == 404 -> UserFacingException("File not found.")
                else -> UserFacingException("Transfer failed ($code). Try again.")
            }
            if (code >= 500 || code == 429) {
                lastError = TransferInterruptedException(error.message ?: "Network interrupted. Try again.")
                continue
            }
            throw error
        }
        throw lastError
    }

    private fun headersOf(target: TransferTargetDto) = okhttp3.Headers.Builder().apply {
        target.headers.forEach { (name, value) -> add(name, value) }
    }.build()

    /** Display name and size of a picked document, when the provider knows them. */
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

    /** API steps retry server hiccups; the final send is idempotent server-side, so a retry can't duplicate it. */
    private suspend fun <T> retryTransient(block: suspend () -> T): T {
        var attempt = 0
        while (true) {
            try {
                return block()
            } catch (e: ApiException) {
                val transient = e.code == "network_error" || e.status >= 500 || (e.status == 409 && e.code == "upload_incomplete")
                if (!transient || ++attempt >= MAX_ATTEMPTS) throw e
                delay(400L shl (attempt - 1))
            }
        }
    }

    /** Turns failures into the web client's user-facing wording (fileErrorMessage in actions.ts). */
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
            if (e.status == 401) throw e // lets the session end itself
            throw UserFacingException(
                when {
                    e.code == "file_deleted" -> "File deleted."
                    e.code == "file_expired" -> "File expired."
                    e.code == "upload_expired" -> "Upload took too long. Try again."
                    e.code in QUOTA_CODES -> e.message ?: fallback
                    e.code == "file_sharing_disabled" -> "File sharing isn't available on this server."
                    e.code == "network_error" || e.code == "tor_unavailable" || e.status >= 500 ->
                        throw TransferInterruptedException(if (e.code == "tor_unavailable") TOR_UNAVAILABLE_MESSAGE else "Network interrupted. Try again.")
                    e.status == 400 && e.message?.contains("too large", ignoreCase = true) == true -> "File too large."
                    e.status == 404 -> "File not found."
                    else -> fallback
                },
            )
        } catch (e: TorUnavailableException) {
            throw TransferInterruptedException(TOR_UNAVAILABLE_MESSAGE)
        } catch (e: IOException) {
            throw TransferInterruptedException()
        }
    }

    companion object {
        /** What a voice message is called and typed as inside its (encrypted) descriptor; the web plays audio/mp4. */
        const val VOICE_FILENAME = "Voice message.m4a"
        const val VOICE_MIME_TYPE = "audio/mp4"
        private const val MAX_ATTEMPTS = 4
        // Refusals the server words for the user (plan, storage, rate limits): shown as-is.
        private val QUOTA_CODES = setOf("pro_required", "upload_quota_exceeded", "too_many_pending_uploads", "rate_limited", "storage_limit_exceeded")
    }
}

/** A file picked to send, before it's read: what the Tor outbox needs to queue it. */
class PickedFile(val name: String, val mimeType: String, val size: Long?, val open: () -> java.io.InputStream)
