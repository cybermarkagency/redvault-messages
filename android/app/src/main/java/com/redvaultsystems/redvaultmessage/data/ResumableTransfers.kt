// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodium
import com.redvaultsystems.redvaultmessage.crypto.CryptoException
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject
import okhttp3.Call
import okhttp3.Callback
import okhttp3.Response
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.security.MessageDigest
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/**
 * Small records sealed with XChaCha20-Poly1305 under one key, each bound to
 * its name by the associated data so two records can't be swapped on disk.
 * The Files tab's upload journal uses it (key: crypto_kdf "rvxfer01", 1, the
 * same derivation as the desktop app's DriveRepository::deriveJournalKey).
 */
class SealedRecords(private val sodium: LazySodium, private val key: ByteArray) {
    fun seal(plaintext: ByteArray, associated: String): ByteArray {
        val nonce = sodium.randomBytesBuf(NONCE_BYTES)
        val ad = associated.toByteArray(Charsets.UTF_8)
        val sealed = ByteArray(plaintext.size + TAG_BYTES)
        val length = LongArray(1)
        if (!sodium.cryptoAeadXChaCha20Poly1305IetfEncrypt(sealed, length, plaintext, plaintext.size.toLong(), ad, ad.size.toLong(), null, nonce, key)) {
            throw CryptoException("Sealing failed")
        }
        return nonce + sealed.copyOf(length[0].toInt())
    }

    /** Null when it doesn't open (another account's key, damaged, or moved). */
    fun open(sealed: ByteArray, associated: String): ByteArray? {
        if (sealed.size < NONCE_BYTES + TAG_BYTES) return null
        val ad = associated.toByteArray(Charsets.UTF_8)
        val body = sealed.copyOfRange(NONCE_BYTES, sealed.size)
        val plaintext = ByteArray(body.size - TAG_BYTES)
        val length = LongArray(1)
        val ok = sodium.cryptoAeadXChaCha20Poly1305IetfDecrypt(
            plaintext, length, null, body, body.size.toLong(), ad, ad.size.toLong(), sealed.copyOfRange(0, NONCE_BYTES), key,
        )
        return if (ok) plaintext.copyOf(length[0].toInt()) else null
    }

    fun close() = key.fill(0)

    companion object {
        private const val NONCE_BYTES = 24
        private const val TAG_BYTES = 16

        /** crypto_kdf_derive_from_key(32, 1, "rvxfer01", account private key). */
        fun deriveTransferKey(sodium: LazySodium, accountPrivateKey: ByteArray): ByteArray {
            val key = ByteArray(32)
            if (sodium.cryptoKdfDeriveFromKey(key, key.size, 1L, "rvxfer01".toByteArray(Charsets.US_ASCII), accountPrivateKey) != 0) {
                throw CryptoException("Transfer key derivation failed")
            }
            return key
        }
    }
}

/** An unfinished resumable upload, as its sealed journal records it (it holds the file's key). */
@Serializable
data class UploadJournal(
    val objectId: String,
    val name: String,
    val parent: String,
    val mimeType: String,
    val size: Long,
    val modifiedMs: Long,
    val fileId: String,
    val version: Int,
    val meta: JsonObject,
    val encryptedSize: Long,
    val partSize: Long,
    val partCount: Int,
    /** Each part's SHA-256, lowercase hex, in order. */
    val partSha256: List<String>,
    val createdMs: Long,
)

/** A resumable upload that didn't finish (no connection, Tor down, the app closed). */
data class PendingUpload(val objectId: String, val name: String, val parent: String, val size: Long, val partCount: Int)

internal object Parts {
    /** Part [n]'s length (1-based): every part is [partSize] bytes except the last, which is the rest. */
    fun length(total: Long, partSize: Long, partCount: Int, n: Int): Long =
        if (n < partCount) partSize else total - partSize * (partCount - 1)

    /** Each part's SHA-256 (hex), and the manifest: SHA-256 over all the raw part digests in order. */
    fun hashes(file: File, total: Long, partSize: Long, partCount: Int): Pair<List<String>, String> {
        val manifest = MessageDigest.getInstance("SHA-256")
        val hashes = ArrayList<String>(partCount)
        file.inputStream().buffered(1 shl 16).use { input ->
            val buffer = ByteArray(1 shl 16)
            for (n in 1..partCount) {
                val digest = MessageDigest.getInstance("SHA-256")
                var left = length(total, partSize, partCount, n)
                while (left > 0) {
                    val read = input.read(buffer, 0, minOf(buffer.size.toLong(), left).toInt())
                    if (read < 0) throw IOException("The encrypted copy is shorter than expected")
                    digest.update(buffer, 0, read)
                    left -= read
                }
                val raw = digest.digest()
                manifest.update(raw)
                hashes += hex(raw)
            }
        }
        return hashes to hex(manifest.digest())
    }

    /** Reads part [n] of [file] into memory (at most the part size, 8 MiB). */
    fun read(file: File, total: Long, partSize: Long, partCount: Int, n: Int): ByteArray {
        val length = length(total, partSize, partCount, n)
        val bytes = ByteArray(length.toInt())
        RandomAccessFile(file, "r").use { raf ->
            raf.seek(partSize * (n - 1))
            raf.readFully(bytes)
        }
        return bytes
    }

    fun sha256Hex(bytes: ByteArray): String = hex(MessageDigest.getInstance("SHA-256").digest(bytes))

    private fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }
}

/** Runs the call without blocking a thread, and cancels it (closing its socket) when the coroutine is cancelled. */
internal suspend fun Call.await(): Response = suspendCancellableCoroutine { cont ->
    cont.invokeOnCancellation { cancel() }
    enqueue(object : Callback {
        override fun onFailure(call: Call, e: IOException) {
            if (cont.isActive) cont.resumeWithException(e)
        }

        override fun onResponse(call: Call, response: Response) {
            if (cont.isActive) cont.resume(response) else response.close()
        }
    })
}
