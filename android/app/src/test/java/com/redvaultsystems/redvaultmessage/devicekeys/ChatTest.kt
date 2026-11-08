// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileEncryptionInfo
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom

/** Section 9 of docs/multi-device-threat-model.md on the JVM: the real engine, an in-memory server. */
class ChatTest {
    @get:Rule val folder = TemporaryFolder()

    private lateinit var engine: EngineRunner
    private val server = FakeV3Server()
    private val alice = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val bob = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    private val carol = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
    private val chat = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"
    private val group = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"

    @Before fun load() {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        engine = EngineRunner(call!!)
        server.participants[chat] = listOf(alice, bob)
        server.participants[group] = listOf(alice, bob, carol)
    }

    private fun legacy() = ByteArray(32).also { SecureRandom().nextBytes(it) }
    private fun storage(name: String) = DeviceStorage(File(folder.root, name), SoftwareKeyWrapper())
    private fun history(name: String, device: V3Device) = EncryptedChatHistory(File(folder.root, "$name.hist"), device.historyKey())

    private suspend fun first(user: String, name: String) = setUpFirstDevice(user, legacy(), storage(name), engine, server.api(user))

    /** A second device for the same account, restored from its recovery key (pairing itself is tested in DeviceKeysTest). */
    private suspend fun second(user: String, name: String, existing: V3Device): V3Device {
        val key = createRecoveryBackup(existing)
        return restoreWithRecoveryKey(user, key, storage(name), engine, server.api(user))
    }

    @Test fun `a one-to-one message reaches the contact and the sender's other device`() = runBlocking {
        val a1 = first(alice, "a1")
        val a2 = second(alice, "a2", a1)
        val b1 = first(bob, "b1")
        val ha1 = history("a1", a1)
        val ha2 = history("a2", a2)
        val hb1 = history("b1", b1)

        val outcome = sendText(a1, ha1, chat, bob, "hello bob")
        assertTrue(outcome is SendOutcome.Sent)
        // One ciphertext for B's device and one for A's other device; none for the sender.
        assertEquals(1, server.pending(bob, b1.deviceId))
        assertEquals(1, server.pending(alice, a2.deviceId))
        assertEquals(0, server.pending(alice, a1.deviceId))
        assertTrue(server.sent.none { it.contains("hello bob") })

        val got = receiveMessages(b1, hb1)
        assertEquals(listOf("hello bob"), got.received.map { it.text })
        assertEquals("in", got.received.single().direction)
        assertEquals(0, server.pending(bob, b1.deviceId))

        val copy = receiveMessages(a2, ha2)
        assertEquals("out", copy.received.single().direction)
        assertEquals("sent", copy.received.single().status)

        // Replies work in the other direction, on an existing session.
        sendText(b1, hb1, chat, alice, "hi alice")
        assertEquals(listOf("hi alice"), receiveMessages(a1, ha1).received.map { it.text })
        assertEquals(listOf("hi alice"), receiveMessages(a2, ha2).received.map { it.text })
        // Receiving twice shows nothing new.
        assertTrue(receiveMessages(a1, ha1).received.isEmpty())
        assertEquals(2, ha1.messages(chat).size)
    }

    @Test fun `a chat with no device keys stays on protocol 1 until it is pinned, then refuses`() = runBlocking {
        val a1 = first(alice, "a1")
        val ha = history("a1", a1)
        assertTrue(sendText(a1, ha, chat, bob, "hi") is SendOutcome.Protocol1)
        assertFalse(ha.pinned(chat))
        ha.pin(chat)
        val error = runCatching { sendText(a1, ha, chat, bob, "hi") }.exceptionOrNull()
        assertTrue(error is EngineException && error.code == "contact_has_no_devices")
    }

    @Test fun `a list that changed since it was read is refetched once`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val ha = history("a1", a1)
        server.staleNext = true
        assertTrue(sendText(a1, ha, chat, bob, "after a stale list") is SendOutcome.Sent)
        assertEquals(listOf("after a stale list"), receiveMessages(b1, history("b1", b1)).received.map { it.text })
    }

    @Test fun `a failed send is kept as failed and the retry reuses its id`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val ha = history("a1", a1)
        val id = "11111111-2222-4333-8444-555555555555"
        server.failNext = true
        val error = runCatching { sendText(a1, ha, chat, bob, "retry me", id = id) }.exceptionOrNull()
        assertTrue(error is com.redvaultsystems.redvaultmessage.data.api.ApiException)
        assertEquals("failed", ha.messages(chat).single().status)
        assertEquals(0, server.pending(bob, b1.deviceId))
        sendText(a1, ha, chat, bob, "retry me", id = id)
        assertEquals(1, ha.messages(chat).size)
        assertEquals("sent", ha.messages(chat).single().status)
        assertEquals(1, receiveMessages(b1, history("b1", b1)).received.size)
    }

    @Test fun `history is encrypted at rest and readable again with the same key`() = runBlocking {
        val a1 = first(alice, "a1")
        val ha = history("a1", a1)
        ha.put(ChatRecord(chat, "id-1", alice, a1.deviceId, 1, 1, "a secret sentence", "out", "sent"))
        ha.pin(chat)
        val bytes = File(folder.root, "a1.hist").readBytes()
        assertFalse(String(bytes, Charsets.ISO_8859_1).contains("a secret sentence"))
        val again = EncryptedChatHistory(File(folder.root, "a1.hist"), a1.historyKey())
        assertEquals("a secret sentence", again.messages(chat).single().text)
        assertTrue(again.pinned(chat))
        val wrong = EncryptedChatHistory(File(folder.root, "a1.hist"), ByteArray(32))
        assertTrue(wrong.messages(chat).isEmpty())
    }

    private val descriptor = FileDescriptor(
        "file", 1, "33333333-3333-4333-8333-333333333333", "c2VjcmV0LWZpbGUta2V5LXNlY3JldC1maWxlLWtleSE=", "notes.txt", "text/plain", 5, 46, "a".repeat(64),
        FileEncryptionInfo("xchacha20poly1305-secretstream", 1, "aGVhZGVyaGVhZGVyaGVhZGVyaGVhZGVy", 65536),
    )

    @Test fun `reactions, edits and deletes reach the contact and the sender's other device`() = runBlocking {
        val a1 = first(alice, "a1")
        val a2 = second(alice, "a2", a1)
        val b1 = first(bob, "b1")
        val ha1 = history("a1", a1)
        val ha2 = history("a2", a2)
        val hb1 = history("b1", b1)

        val id = (sendText(a1, ha1, chat, bob, "first words") as SendOutcome.Sent).record.id
        receiveMessages(b1, hb1)
        receiveMessages(a2, ha2)

        sendChatOp(a1, ha1, chat, false, bob, ChatOp.edit(id, "better words"))
        sendChatOp(b1, hb1, chat, false, alice, ChatOp.reaction(id, "🔥"))
        receiveMessages(a1, ha1)
        receiveMessages(a2, ha2)
        receiveMessages(b1, hb1)
        for (h in listOf(ha1, ha2, hb1)) {
            val shown = foldChat(h.messages(chat)).single()
            assertEquals("better words", shown.record.text)
            assertEquals(listOf(ShownReaction(bob, "🔥")), shown.reactions)
        }
        assertTrue(server.sent.none { it.contains("better words") })

        sendChatOp(a1, ha1, chat, false, bob, ChatOp.delete(id))
        receiveMessages(a2, ha2)
        receiveMessages(b1, hb1)
        for (h in listOf(ha1, ha2, hb1)) {
            assertTrue(foldChat(h.messages(chat)).single().deleted)
            // The message and the delete; the edit and the reaction are gone.
            assertEquals(2, h.messages(chat).size)
            assertTrue(h.messages(chat).all { it.text.isEmpty() })
        }
    }

    @Test fun `a file's key reaches the other side only inside the envelope, and a delete takes it away`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val ha = history("a1", a1)
        val hb = history("b1", b1)

        val id = (sendChatFile(a1, ha, chat, false, bob, descriptor) as SendOutcome.Sent).record.id
        assertTrue(server.sent.none { it.contains(descriptor.fileKey) })
        assertEquals(listOf(descriptor), receiveMessages(b1, hb).received.map { it.file })

        sendChatOp(a1, ha, chat, false, bob, ChatOp.delete(id))
        receiveMessages(b1, hb)
        for (h in listOf(ha, hb)) assertNull(h.messages(chat).first { it.id == id }.file)
    }

    @Test fun `group reactions, edits, deletes and files work too`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val ha = history("a1", a1)
        val hb = history("b1", b1)

        val id = (sendGroupText(a1, ha, group, "to the group") as SendOutcome.Sent).record.id
        val fileId = (sendChatFile(a1, ha, group, true, "", descriptor) as SendOutcome.Sent).record.id
        sendChatOp(a1, ha, group, true, "", ChatOp.edit(id, "fixed"))
        sendChatOp(a1, ha, group, true, "", ChatOp.delete(fileId))
        receiveMessages(b1, hb)
        val shown = foldChat(hb.messages(group))
        assertEquals("fixed", shown.first { it.record.id == id }.record.text)
        assertTrue(shown.first { it.record.id == fileId }.deleted)
        assertNull(hb.messages(group).first { it.id == fileId }.file)
    }

    @Test fun `a failed op isn't kept, a failed file is, and an op to a contact with no devices is refused`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val ha = history("a1", a1)
        val target = "11111111-2222-4333-8444-555555555555"

        server.failNext = true
        assertTrue(runCatching { sendChatOp(a1, ha, chat, false, bob, ChatOp.reaction(target, "👍")) }.isFailure)
        assertTrue(ha.messages(chat).isEmpty())

        server.failNext = true
        assertTrue(runCatching { sendChatFile(a1, ha, chat, false, bob, descriptor, id = target) }.isFailure)
        assertEquals(descriptor, ha.failed().single().file)
        assertTrue(sendChatFile(a1, ha, chat, false, bob, descriptor, id = target) is SendOutcome.Sent)
        assertTrue(ha.failed().isEmpty())
        assertEquals(listOf(descriptor), receiveMessages(b1, history("b1", b1)).received.map { it.file })

        val error = runCatching { sendChatOp(a1, ha, chat, false, carol, ChatOp.reaction(target, "👍")) }.exceptionOrNull()
        assertTrue(error is EngineException)
    }

    @Test fun `whether a chat is on device keys is known before a file is uploaded`() = runBlocking {
        val a1 = first(alice, "a1")
        val ha = history("a1", a1)
        assertFalse(contactOnDeviceKeys(a1, ha, chat, bob))
        ha.pin(chat)
        val error = runCatching { contactOnDeviceKeys(a1, ha, chat, bob) }.exceptionOrNull()
        assertTrue(error is EngineException && error.code == "contact_has_no_devices")
        first(bob, "b1")
        assertTrue(contactOnDeviceKeys(a1, ha, chat, bob))
    }

    @Test fun `a group message reaches every member device, with keys shared once`() = runBlocking {
        val a1 = first(alice, "a1")
        val a2 = second(alice, "a2", a1)
        val b1 = first(bob, "b1")
        val c1 = first(carol, "c1")
        val ha1 = history("a1", a1)
        val ha2 = history("a2", a2)
        val hb1 = history("b1", b1)
        val hc1 = history("c1", c1)

        assertTrue(sendGroupText(a1, ha1, group, "hello group") is SendOutcome.Sent)
        // A room key and the group message for each of the three other devices.
        assertEquals(2, server.pending(bob, b1.deviceId))
        assertEquals(2, server.pending(carol, c1.deviceId))
        assertEquals(2, server.pending(alice, a2.deviceId))
        assertTrue(server.sent.none { it.contains("hello group") })

        for ((device, h) in listOf(b1 to hb1, c1 to hc1, a2 to ha2)) {
            val report = receiveMessages(device, h)
            assertEquals(listOf("hello group"), report.received.map { it.text })
            assertEquals(0, server.pending(device.userId, device.deviceId))
        }
        assertEquals("out", ha2.messages(group).single().direction)

        // A second message shares no key again: one row per device.
        sendGroupText(a1, ha1, group, "again")
        assertEquals(1, server.pending(bob, b1.deviceId))
        assertEquals(listOf("again"), receiveMessages(b1, hb1).received.map { it.text })

        // Bob's reply opens his own session; everyone, including Alice's two phones, gets it.
        sendGroupText(b1, hb1, group, "hi all")
        assertEquals(listOf("hi all"), receiveMessages(a1, ha1).received.map { it.text })
        assertEquals(listOf("again", "hi all"), receiveMessages(c1, hc1).received.map { it.text })
        receiveMessages(a2, ha2)
        assertEquals(setOf("hello group", "again", "hi all"), ha2.messages(group).map { it.text }.toSet())
    }
}
