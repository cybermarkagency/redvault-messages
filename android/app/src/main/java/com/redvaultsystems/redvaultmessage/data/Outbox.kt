// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodium
import com.redvaultsystems.redvaultmessage.crypto.CryptoException
import com.redvaultsystems.redvaultmessage.crypto.EncryptedFile
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import kotlinx.serialization.Serializable
import java.io.File
import java.io.InputStream
import java.util.Base64
import java.util.UUID

/** One message waiting to go out over Tor. Its [id] is also the server's clientMessageId (db/migrations/033). */
@Serializable
data class OutboxItem(
    val id: String,
    val conversationId: String,
    /** "text", "file" or "voice". */
    val kind: String,
    /** The message text, or the file's name. */
    val text: String,
    val replyToId: String? = null,
    val replyToSender: String? = null,
    val replyPreview: String? = null,
    val replyFromMe: Boolean = false,
    val mimeType: String? = null,
    /** Files: the queued copy's key and header (FileCrypto), and its sizes and digest. */
    val fileKey: String? = null,
    val fileHeader: String? = null,
    val plaintextSize: Long = 0,
    val encryptedSize: Long = 0,
    val ciphertextSha256: String? = null,
    val createdAtMs: Long,
    /** Send order: strictly increasing, even for items queued in the same millisecond. */
    val order: Long,
    val attempts: Int = 0,
    /** The last failure, safe to show. */
    val lastError: String? = null,
    /** The server refused it (not a network problem): it waits for Retry or Discard instead of going out again. */
    val held: Boolean = false,
)

/**
 * Messages, photos, files and voice messages waiting for Tor
 * (docs/tor-routing-plan.md): they go out only through the Tor gate, and
 * wait here, sealed, while it's down, across restarts, until sent. The same
 * design as the desktop app's data/outbox:
 *
 * - each item is a JSON record sealed with XChaCha20-Poly1305 under a key
 *   derived from the account's private key (crypto_kdf, "rvoutbx1", 1), with
 *   "outbox-item:<id>" as associated data, so records can't be swapped;
 * - a file's queued copy is the FileCrypto ciphertext (secretstream, its own
 *   random key inside the sealed record): no plaintext is ever stored;
 * - an item leaves the outbox only once the server has stored the message,
 *   and its id is the clientMessageId, so a send repeated after a lost answer
 *   or a crash is never stored twice.
 */
class Outbox(
    private val dir: File,
    private val key: ByteArray,
    private val sodium: LazySodium,
    private val fileCrypto: FileCrypto,
) {
    private var lastOrder = 0L

    init {
        dir.mkdirs()
        lastOrder = items().maxOfOrNull { it.order } ?: 0L
    }

    /** Oldest first. Records that don't open (another account's, or damaged) are skipped. */
    @Synchronized
    fun items(): List<OutboxItem> =
        (dir.listFiles { f -> f.name.endsWith(ITEM_SUFFIX) } ?: emptyArray())
            .mapNotNull { file -> runCatching { read(file.name.removeSuffix(ITEM_SUFFIX)) }.getOrNull() }
            .sortedBy { it.order }

    @Synchronized
    fun count(): Int = items().size

    fun addText(conversationId: String, text: String, reply: ReplyTarget? = null): OutboxItem = put(
        OutboxItem(
            id = newId(), conversationId = conversationId, kind = "text", text = text,
            replyToId = reply?.ref?.messageId, replyToSender = reply?.ref?.senderId, replyPreview = reply?.ref?.preview,
            replyFromMe = reply?.fromMe ?: false, createdAtMs = System.currentTimeMillis(), order = nextOrder(),
        ),
    )

    /** Encrypts [source] into the outbox (it can be deleted afterwards) and queues it. */
    fun addFile(conversationId: String, kind: String, name: String, mimeType: String, source: () -> InputStream): OutboxItem {
        val id = newId()
        val copy = File(dir, id + FILE_SUFFIX)
        val encrypted: EncryptedFile = try {
            source().use { fileCrypto.encryptFile(it, copy) }
        } catch (e: Throwable) {
            copy.delete()
            throw e
        }
        try {
            return put(
                OutboxItem(
                    id = id, conversationId = conversationId, kind = kind, text = name, mimeType = mimeType,
                    fileKey = b64(encrypted.key), fileHeader = b64(encrypted.header),
                    plaintextSize = encrypted.plaintextSize, encryptedSize = encrypted.encryptedSize,
                    ciphertextSha256 = encrypted.ciphertextSha256,
                    createdAtMs = System.currentTimeMillis(), order = nextOrder(),
                ),
            )
        } finally {
            encrypted.key.fill(0)
        }
    }

    /** Decrypts a file item's queued copy into [destination] (checked against its digest), for sending. */
    fun openFile(item: OutboxItem, destination: File) {
        val key = Base64.getDecoder().decode(item.fileKey ?: throw CryptoException("Not a file item"))
        try {
            File(dir, item.id + FILE_SUFFIX).inputStream().buffered().use { input ->
                destination.outputStream().buffered().use { output ->
                    fileCrypto.decrypt(
                        input = input, output = output, key = key,
                        header = Base64.getDecoder().decode(item.fileHeader),
                        encryptedSize = item.encryptedSize, ciphertextSha256 = item.ciphertextSha256 ?: "",
                    )
                }
            }
        } catch (e: Throwable) {
            destination.delete()
            throw e
        } finally {
            key.fill(0)
        }
    }

    /** One more failed attempt; [hold] keeps it from going out again until [release]. */
    @Synchronized
    fun recordAttempt(id: String, error: String, hold: Boolean = false) {
        val item = runCatching { read(id) }.getOrNull() ?: return
        put(item.copy(attempts = item.attempts + 1, lastError = error, held = item.held || hold))
    }

    /** Retry: a held item goes out again with the next drain. */
    @Synchronized
    fun release(id: String) {
        val item = runCatching { read(id) }.getOrNull() ?: return
        put(item.copy(held = false, lastError = null))
    }

    /** The session ended: the key is overwritten (the sealed records stay for the next sign-in). */
    @Synchronized
    fun close() {
        key.fill(0)
    }

    /** Sent (the server stored it) or discarded. */
    @Synchronized
    fun remove(id: String) {
        File(dir, id + ITEM_SUFFIX).delete()
        File(dir, id + FILE_SUFFIX).delete()
    }

    @Synchronized
    private fun put(item: OutboxItem): OutboxItem {
        val plaintext = ApiClient.json.encodeToString(OutboxItem.serializer(), item).toByteArray(Charsets.UTF_8)
        val nonce = sodium.randomBytesBuf(NONCE_BYTES)
        val ad = (AD_PREFIX + item.id).toByteArray(Charsets.UTF_8)
        val sealed = ByteArray(plaintext.size + TAG_BYTES)
        val sealedLen = LongArray(1)
        val ok = sodium.cryptoAeadXChaCha20Poly1305IetfEncrypt(
            sealed, sealedLen, plaintext, plaintext.size.toLong(), ad, ad.size.toLong(), null, nonce, key,
        )
        plaintext.fill(0)
        if (!ok) throw CryptoException("Outbox sealing failed")
        // Written aside and renamed: a crash mid-write never leaves a torn record.
        val target = File(dir, item.id + ITEM_SUFFIX)
        val temp = File(dir, item.id + ITEM_SUFFIX + ".tmp")
        temp.writeBytes(nonce + sealed.copyOf(sealedLen[0].toInt()))
        if (!temp.renameTo(target)) {
            target.delete()
            if (!temp.renameTo(target)) throw java.io.IOException("Couldn't save the outbox")
        }
        return item
    }

    private fun read(id: String): OutboxItem {
        val raw = File(dir, id + ITEM_SUFFIX).readBytes()
        if (raw.size < NONCE_BYTES + TAG_BYTES) throw CryptoException("Outbox record too short")
        val nonce = raw.copyOfRange(0, NONCE_BYTES)
        val sealed = raw.copyOfRange(NONCE_BYTES, raw.size)
        val ad = (AD_PREFIX + id).toByteArray(Charsets.UTF_8)
        val plaintext = ByteArray(sealed.size - TAG_BYTES)
        val plaintextLen = LongArray(1)
        val ok = sodium.cryptoAeadXChaCha20Poly1305IetfDecrypt(
            plaintext, plaintextLen, null, sealed, sealed.size.toLong(), ad, ad.size.toLong(), nonce, key,
        )
        if (!ok) throw CryptoException("Outbox record doesn't open")
        try {
            val item = ApiClient.json.decodeFromString(OutboxItem.serializer(), String(plaintext, 0, plaintextLen[0].toInt(), Charsets.UTF_8))
            if (item.id != id) throw CryptoException("Outbox record moved")
            return item
        } finally {
            plaintext.fill(0)
        }
    }

    @Synchronized
    private fun nextOrder(): Long {
        lastOrder = maxOf(System.currentTimeMillis() * 1000, lastOrder + 1)
        return lastOrder
    }

    private fun newId(): String = UUID.randomUUID().toString()

    private fun b64(bytes: ByteArray): String = Base64.getEncoder().encodeToString(bytes)

    companion object {
        private const val ITEM_SUFFIX = ".item"
        private const val FILE_SUFFIX = ".bin"
        private const val AD_PREFIX = "outbox-item:"
        private const val NONCE_BYTES = 24
        private const val TAG_BYTES = 16
        private const val KDF_CONTEXT = "rvoutbx1"
        private const val KDF_SUBKEY_ID = 1L

        /** crypto_kdf_derive_from_key(32, 1, "rvoutbx1", account private key): the same as the desktop app. */
        fun deriveKey(sodium: LazySodium, accountPrivateKey: ByteArray): ByteArray {
            val key = ByteArray(32)
            if (sodium.cryptoKdfDeriveFromKey(key, key.size, KDF_SUBKEY_ID, KDF_CONTEXT.toByteArray(Charsets.US_ASCII), accountPrivateKey) != 0) {
                throw CryptoException("Outbox key derivation failed")
            }
            return key
        }
    }
}
