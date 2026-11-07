package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.MessageDigest
import kotlin.random.Random

/** The Files tab's resumable uploads: part sizes, digests and the manifest the server checks, and the sealed journal. */
class ResumableTransfersTest {
    @get:Rule val temp = TemporaryFolder()
    private val sodium = LazySodiumJava(SodiumJava())

    @Test
    fun partsCoverTheFileExactly() {
        val part = DriveRepository.PART_SIZE
        for (total in listOf(17L, part - 1, part, part + 1, 3 * part + 12345)) {
            val count = maxOf(1L, (total + part - 1) / part).toInt()
            assertEquals(total, (1..count).sumOf { Parts.length(total, part, count, it) })
        }
    }

    @Test
    fun hashesAndManifestMatchWhatTheServerComputes() {
        val total = 2 * DriveRepository.PART_SIZE + 4321
        val bytes = ByteArray(total.toInt()).also { Random(3).nextBytes(it) }
        val file = File(temp.root, "c").apply { writeBytes(bytes) }
        val count = 3
        val (hashes, manifest) = Parts.hashes(file, total, DriveRepository.PART_SIZE, count)
        // server/src/services/backupService.ts: SHA-256 over the concatenated raw part digests.
        val raw = (1..count).map { n ->
            val start = (n - 1) * DriveRepository.PART_SIZE
            MessageDigest.getInstance("SHA-256").digest(bytes.copyOfRange(start.toInt(), (start + Parts.length(total, DriveRepository.PART_SIZE, count, n)).toInt()))
        }
        assertEquals(raw.map { it.joinToString("") { b -> "%02x".format(b) } }, hashes)
        val expected = MessageDigest.getInstance("SHA-256").digest(raw.fold(ByteArray(0)) { a, b -> a + b }).joinToString("") { "%02x".format(it) }
        assertEquals(expected, manifest)
        // Each part read back is the right slice.
        assertEquals(hashes[2], Parts.sha256Hex(Parts.read(file, total, DriveRepository.PART_SIZE, count, 3)))
    }

    @Test
    fun theJournalOpensOnlyWithItsKeyAndUnderItsOwnName() {
        val key = SealedRecords.deriveTransferKey(sodium, ByteArray(32) { 5 })
        val records = SealedRecords(sodium, key.copyOf())
        val plain = "{\"objectId\":\"abc\"}".toByteArray()
        val sealed = records.seal(plain, "drive-upload:abc")
        assertArrayEquals(plain, records.open(sealed, "drive-upload:abc"))
        assertNull("another upload's name", records.open(sealed, "drive-upload:def"))
        assertNull("another account's key", SealedRecords(sodium, SealedRecords.deriveTransferKey(sodium, ByteArray(32) { 6 })).open(sealed, "drive-upload:abc"))
        val tampered = sealed.copyOf().also { it[it.size - 1] = (it[it.size - 1].toInt() xor 1).toByte() }
        assertNull(records.open(tampered, "drive-upload:abc"))
    }

    @Test
    fun theTransferKeyMatchesTheDesktopDerivation() {
        val account = ByteArray(32) { (it * 3).toByte() }
        val expected = ByteArray(32)
        sodium.cryptoKdfDeriveFromKey(expected, 32, 1L, "rvxfer01".toByteArray(), account)
        assertArrayEquals(expected, SealedRecords.deriveTransferKey(sodium, account))
    }
}
