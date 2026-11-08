// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import com.goterl.lazysodium.LazySodium
import com.goterl.lazysodium.interfaces.SecretStream
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.security.MessageDigest

// Port of ui/src/lib/fileCrypto.ts, format version 1: libsodium's
// crypto_secretstream_xchacha20poly1305 over 64 KiB plaintext chunks, with
// a fresh random key and header per file. The last chunk carries
// TAG_FINAL, so truncating, reordering, dropping or appending chunks all
// fail authentication. The key and header travel only inside the E2EE
// message; the server only sees the ciphertext and its SHA-256.
//
// Both directions stream one chunk at a time between files, so a large
// file is never held in memory whole.

class FileDecryptException(message: String = "Unable to decrypt this file. It may be corrupted or tampered with.") : Exception(message)

class EncryptedFile(
    val key: ByteArray,
    val header: ByteArray,
    val plaintextSize: Long,
    val encryptedSize: Long,
    /** Lowercase hex SHA-256 of the ciphertext. */
    val ciphertextSha256: String,
)

class FileCrypto(private val sodium: LazySodium) {

    /** Encrypts [input] into [output], returning what the recipient needs to decrypt it. */
    fun encrypt(input: InputStream, output: OutputStream, onProgress: (plaintextBytes: Long) -> Unit = {}): EncryptedFile {
        val key = ByteArray(SecretStream.KEYBYTES).also(sodium::cryptoSecretStreamKeygen)
        val header = ByteArray(SecretStream.HEADERBYTES)
        val state = SecretStream.State()
        if (!sodium.cryptoSecretStreamInitPush(state, header, key)) throw IllegalStateException("secretstream init failed")

        val digest = MessageDigest.getInstance("SHA-256")
        val plain = ByteArray(CHUNK_BYTES)
        val cipher = ByteArray(CHUNK_BYTES + CHUNK_OVERHEAD_BYTES)
        val cipherLen = LongArray(1)
        var plaintextSize = 0L
        var encryptedSize = 0L

        // One chunk of look-ahead: a chunk can only be tagged FINAL once we
        // know nothing follows it. An empty file is one empty FINAL chunk.
        var current = readFully(input, plain)
        val next = ByteArray(CHUNK_BYTES)
        try {
            while (true) {
                val nextLen = if (current == CHUNK_BYTES) readFully(input, next) else 0
                val isLast = nextLen == 0
                val tag = if (isLast) SecretStream.TAG_FINAL else SecretStream.TAG_MESSAGE
                if (!sodium.cryptoSecretStreamPush(state, cipher, cipherLen, plain, current.toLong(), tag)) {
                    throw IllegalStateException("secretstream push failed")
                }
                val written = cipherLen[0].toInt()
                digest.update(cipher, 0, written)
                output.write(cipher, 0, written)
                plaintextSize += current
                encryptedSize += written
                onProgress(plaintextSize)
                if (isLast) break
                next.copyInto(plain, 0, 0, nextLen)
                current = nextLen
            }
        } finally {
            plain.fill(0)
            next.fill(0)
        }
        return EncryptedFile(key, header, plaintextSize, encryptedSize, hex(digest.digest()))
    }

    /**
     * Decrypts a ciphertext stream into [output]. Throws [FileDecryptException]
     * on any authentication failure, truncation, trailing data, size mismatch
     * or hash mismatch; [output] must then be discarded (the caller deletes it).
     */
    fun decrypt(
        input: InputStream,
        output: OutputStream,
        key: ByteArray,
        header: ByteArray,
        encryptedSize: Long,
        ciphertextSha256: String,
        onProgress: (ciphertextBytes: Long) -> Unit = {},
    ) {
        if (key.size != SecretStream.KEYBYTES || header.size != SecretStream.HEADERBYTES) throw FileDecryptException()
        val state = SecretStream.State()
        if (!sodium.cryptoSecretStreamInitPull(state, header, key)) throw FileDecryptException()

        val digest = MessageDigest.getInstance("SHA-256")
        val frame = CHUNK_BYTES + CHUNK_OVERHEAD_BYTES
        val cipher = ByteArray(frame)
        val plain = ByteArray(CHUNK_BYTES)
        val plainLen = LongArray(1)
        val tag = ByteArray(1)
        var received = 0L
        var finished = false

        try {
            while (true) {
                val len = readFully(input, cipher)
                if (len == 0) break
                if (finished) throw FileDecryptException() // data after TAG_FINAL
                received += len
                if (received > encryptedSize) throw FileDecryptException()
                digest.update(cipher, 0, len)
                // No additional data; lazysodium needs an empty array rather than null here.
                val ok = sodium.cryptoSecretStreamPull(state, plain, plainLen, tag, cipher, len.toLong(), NO_AD, 0L)
                if (!ok) throw FileDecryptException()
                when (tag[0]) {
                    SecretStream.TAG_FINAL -> finished = true
                    SecretStream.TAG_MESSAGE -> {}
                    else -> throw FileDecryptException()
                }
                output.write(plain, 0, plainLen[0].toInt())
                onProgress(received)
            }
        } finally {
            plain.fill(0)
        }
        if (!finished || received != encryptedSize || hex(digest.digest()) != ciphertextSha256.lowercase()) {
            throw FileDecryptException()
        }
    }

    /** Encrypts [source] to [destination] (a temp file) in one call. */
    fun encryptFile(source: InputStream, destination: File, onProgress: (Long) -> Unit = {}): EncryptedFile =
        destination.outputStream().buffered().use { out -> encrypt(source, out, onProgress) }

    companion object {
        /** Must match server/src/services/fileService.ts FILE_CHUNK_BYTES. */
        const val CHUNK_BYTES = 64 * 1024
        const val CHUNK_OVERHEAD_BYTES = 17 // crypto_secretstream_xchacha20poly1305_ABYTES
        const val ALGORITHM = "xchacha20poly1305-secretstream"
        const val FORMAT_VERSION = 1
        private val NO_AD = ByteArray(0)

        fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }

        /** Reads until [buffer] is full or the stream ends; returns the count read. */
        private fun readFully(input: InputStream, buffer: ByteArray): Int {
            var total = 0
            while (total < buffer.size) {
                val read = input.read(buffer, total, buffer.size - total)
                if (read < 0) break
                total += read
            }
            return total
        }
    }
}
