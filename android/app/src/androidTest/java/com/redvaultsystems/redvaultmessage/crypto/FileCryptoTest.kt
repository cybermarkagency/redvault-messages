// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.crypto

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.goterl.lazysodium.LazySodiumAndroid
import com.goterl.lazysodium.SodiumAndroid
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream

/** Pins the Android file encryption to the web client's format (ui/src/lib/fileCrypto.ts). */
@RunWith(AndroidJUnit4::class)
class FileCryptoTest {
    private val files = FileCrypto(LazySodiumAndroid(SodiumAndroid()))

    private fun asset(name: String): ByteArray =
        InstrumentationRegistry.getInstrumentation().context.assets.open(name).use { it.readBytes() }

    private fun pattern(size: Int) = ByteArray(size) { i -> ((i * 31 + 7) and 0xff).toByte() }

    private fun decrypt(ciphertext: ByteArray, e: EncryptedFile): ByteArray {
        val out = ByteArrayOutputStream()
        files.decrypt(ByteArrayInputStream(ciphertext), out, e.key, e.header, e.encryptedSize, e.ciphertextSha256)
        return out.toByteArray()
    }

    @Test
    fun decryptsAFileEncryptedByTheWebClient() {
        val meta = Json.parseToJsonElement(String(asset("web_file_vector.json"))).jsonObject
        val ciphertext = asset("web_file_vector.bin")
        val out = ByteArrayOutputStream()
        files.decrypt(
            ByteArrayInputStream(ciphertext),
            out,
            key = Base64Codec.decode(meta.getValue("key").jsonPrimitive.content),
            header = Base64Codec.decode(meta.getValue("header").jsonPrimitive.content),
            encryptedSize = meta.getValue("encryptedSize").jsonPrimitive.long,
            ciphertextSha256 = meta.getValue("ciphertextSha256").jsonPrimitive.content,
        )
        assertArrayEquals(pattern(meta.getValue("plaintextSize").jsonPrimitive.int), out.toByteArray())
    }

    @Test
    fun roundTripsAcrossChunkBoundaries() {
        for (size in listOf(0, 1, FileCrypto.CHUNK_BYTES - 1, FileCrypto.CHUNK_BYTES, FileCrypto.CHUNK_BYTES + 1, 200_000)) {
            val plain = pattern(size)
            val ciphertext = ByteArrayOutputStream()
            val e = files.encrypt(ByteArrayInputStream(plain), ciphertext)
            val chunks = maxOf(1, (size + FileCrypto.CHUNK_BYTES - 1) / FileCrypto.CHUNK_BYTES)
            // Same size formula as the web's encryptedSizeFor().
            assertEquals("size $size", size.toLong() + chunks * FileCrypto.CHUNK_OVERHEAD_BYTES, e.encryptedSize)
            assertArrayEquals("size $size", plain, decrypt(ciphertext.toByteArray(), e))
        }
    }

    @Test
    fun rejectsTamperingTruncationAndExtraData() {
        val ciphertext = ByteArrayOutputStream()
        val e = files.encrypt(ByteArrayInputStream(pattern(150_000)), ciphertext)
        val good = ciphertext.toByteArray()

        val flipped = good.copyOf().also { it[70_000] = (it[70_000].toInt() xor 1).toByte() }
        assertThrows(FileDecryptException::class.java) { decrypt(flipped, e) }

        val truncated = good.copyOf(FileCrypto.CHUNK_BYTES + FileCrypto.CHUNK_OVERHEAD_BYTES)
        assertThrows(FileDecryptException::class.java) { decrypt(truncated, EncryptedFile(e.key, e.header, 0, truncated.size.toLong(), "0".repeat(64))) }

        val extended = good + ByteArray(10)
        assertThrows(FileDecryptException::class.java) { decrypt(extended, e) }

        assertThrows(FileDecryptException::class.java) {
            decrypt(good, EncryptedFile(e.key, e.header, e.plaintextSize, e.encryptedSize, "f".repeat(64)))
        }
    }
}
