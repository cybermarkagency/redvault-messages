// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.data.model.ReplyRef
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import kotlin.random.Random

/** The Tor outbox (data/Outbox.kt): sealed on disk, in order, kept until sent, and readable only with this account's key. */
class OutboxTest {
    @get:Rule val temp = TemporaryFolder()

    private val sodium = LazySodiumJava(SodiumJava())
    private val accountKey = ByteArray(32) { (it * 7 + 3).toByte() }
    private fun outbox(dir: File = temp.root, key: ByteArray = Outbox.deriveKey(sodium, accountKey)) = Outbox(dir, key, sodium, FileCrypto(sodium))

    @Test
    fun itemsComeBackInOrderAcrossRestarts() {
        val first = outbox()
        val texts = (1..25).map { "message $it" }
        texts.forEach { first.addText("c1", it) }
        val reopened = outbox()
        assertEquals(texts, reopened.items().map { it.text })
        // Same millisecond or not, the order is strictly increasing.
        val orders = reopened.items().map { it.order }
        assertEquals(orders.sorted(), orders)
        assertEquals(orders.size, orders.toSet().size)
    }

    @Test
    fun nothingReadableIsStored() {
        val box = outbox()
        val chat = "5c1e2a7f-0b7d-4e57-a0aa-1f6f3b2d9c11"
        val secret = "the launch code is " + "x".repeat(40)
        box.addText(chat, secret, ReplyTarget(ReplyRef("m-1", "u-2", "a quoted line nobody may read"), fromMe = false))
        val plain = ByteArray(300_000).also { Random(1).nextBytes(it) }
        box.addFile(chat, "file", "holiday.jpg", "image/jpeg") { plain.inputStream() }
        val disk = temp.root.walkTopDown().filter { it.isFile }.map { it.readBytes() }.fold(ByteArray(0)) { a, b -> a + b }
        val text = String(disk, Charsets.ISO_8859_1)
        for (needle in listOf(secret, "a quoted line nobody may read", "holiday.jpg", "image/jpeg", chat)) {
            assertFalse("found \"$needle\" on disk", text.contains(needle))
        }
        assertFalse(String(disk, Charsets.ISO_8859_1).contains(String(plain.copyOfRange(5000, 5064), Charsets.ISO_8859_1)))
    }

    @Test
    fun aQueuedFileComesBackExactly() {
        val box = outbox()
        val plain = ByteArray(1_234_567).also { Random(2).nextBytes(it) }
        val item = box.addFile("c1", "voice", "Voice message.m4a", "audio/mp4") { plain.inputStream() }
        val out = File(temp.newFolder(), "plain")
        outbox().openFile(outbox().items().single { it.id == item.id }, out)
        assertArrayEquals(plain, out.readBytes())
    }

    @Test
    fun aChangedFileCopyIsRefused() {
        val box = outbox()
        val item = box.addFile("c1", "file", "a.bin", "application/octet-stream") { ByteArray(200_000) { 1 }.inputStream() }
        val copy = File(temp.root, item.id + ".bin")
        val bytes = copy.readBytes()
        bytes[bytes.size / 2] = (bytes[bytes.size / 2].toInt() xor 1).toByte()
        copy.writeBytes(bytes)
        val out = File(temp.newFolder(), "plain")
        assertTrue(runCatching { box.openFile(item, out) }.isFailure)
        assertFalse("no partial plaintext left behind", out.exists())
    }

    @Test
    fun anotherAccountsKeyOpensNothing() {
        outbox().addText("c1", "hello")
        val other = outbox(key = Outbox.deriveKey(sodium, ByteArray(32) { 9 }))
        assertTrue(other.items().isEmpty())
    }

    @Test
    fun recordsCantBeSwapped() {
        val box = outbox()
        val a = box.addText("c1", "to alice")
        val b = box.addText("c2", "to bob")
        // Swap the two sealed records on disk: each is bound to its own id, so neither opens.
        val fa = File(temp.root, a.id + ".item")
        val fb = File(temp.root, b.id + ".item")
        val bytesA = fa.readBytes()
        fa.writeBytes(fb.readBytes())
        fb.writeBytes(bytesA)
        assertTrue(outbox().items().isEmpty())
    }

    @Test
    fun attemptsHoldsReleaseAndRemoval() {
        val box = outbox()
        val item = box.addText("c1", "hello")
        box.recordAttempt(item.id, "Tor connection unavailable")
        box.items().single().let {
            assertEquals(1, it.attempts)
            assertFalse(it.held)
        }
        box.recordAttempt(item.id, "Refused", hold = true)
        assertTrue(box.items().single().held)
        box.release(item.id)
        box.items().single().let {
            assertFalse(it.held)
            assertEquals(null, it.lastError)
        }
        box.remove(item.id)
        assertTrue(outbox().items().isEmpty())
        assertTrue(temp.root.listFiles()!!.isEmpty())
    }

    @Test
    fun theIdIsAValidClientMessageId() {
        val id = outbox().addText("c1", "hello").id
        // server/src/routes/conversationRoutes.ts: ^[A-Za-z0-9_-]{16,64}$
        assertTrue(Regex("^[A-Za-z0-9_-]{16,64}$").matches(id))
    }

    @Test
    fun theKeyMatchesTheDesktopDerivation() {
        // crypto_kdf_derive_from_key(32, 1, "rvoutbx1", key): two apps of one account derive the same key.
        val key = Outbox.deriveKey(sodium, accountKey)
        val expected = ByteArray(32)
        sodium.cryptoKdfDeriveFromKey(expected, 32, 1L, "rvoutbx1".toByteArray(), accountKey)
        assertArrayEquals(expected, key)
    }
}
