// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileEncryptionInfo
import kotlinx.coroutines.delay
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

/**
 * Part A on device keys end to end (docs/signal-integration/08-part-a-chat-features.md): replies, polls, pins,
 * receipts, disappearing messages and forwarded files through V3Chat, with the real engine and the in-memory server,
 * and the same over the Tor queue while Tor is down.
 */
class V3ChatPartATest {
    @get:Rule val folder = TemporaryFolder()

    private lateinit var engine: EngineRunner
    private val server = FakeV3Server()
    private val alice = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val bob = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    private val carol = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
    private val chat = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"
    private val group = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
    private val wrappers = HashMap<String, SoftwareKeyWrapper>()

    @Before fun load() {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        engine = EngineRunner(call!!)
    }

    private fun legacy() = ByteArray(32).also { SecureRandom().nextBytes(it) }

    private suspend fun account(user: String, name: String): V3Chat {
        val storage = DeviceStorage(File(folder.root, name), SoftwareKeyWrapper())
        val api = server.api(user)
        setUpFirstDevice(user, legacy(), storage, engine, api)
        return V3Chat(DeviceKeysSession(user, ::legacy, storage, engine, api), user, File(folder.root, "$name-history"))
    }

    /** The fake server's API with Tor "down" for the posts only (as in V3ChatTest). */
    private class TorDropsPosts(private val inner: V3Api) : V3Api by inner {
        var down = true
        override suspend fun sendMessages(senderDeviceId: String, conversationId: String?, messages: List<Triple<String, String, String>>, allDevices: Boolean) {
            if (down) throw ApiException(0, "tor_unavailable", "Tor connection unavailable")
            inner.sendMessages(senderDeviceId, conversationId, messages, allDevices)
        }
        override suspend fun sendGroupMessage(senderDeviceId: String, conversationId: String, body: String, recipients: List<Pair<String, String>>, allDevices: Boolean) {
            if (down) throw ApiException(0, "tor_unavailable", "Tor connection unavailable")
            inner.sendGroupMessage(senderDeviceId, conversationId, body, recipients, allDevices)
        }
    }

    private suspend fun queuedAccount(user: String, name: String, tor: TorDropsPosts): V3Chat {
        val wrapper = wrappers.getOrPut(name) { SoftwareKeyWrapper() }
        val storage = DeviceStorage(File(folder.root, name), wrapper)
        setUpFirstDevice(user, legacy(), storage, engine, server.api(user))
        val outgoing = QueueingV3Api(tor, V3SendQueue(File(folder.root, "$name-queue"), user, wrapper))
        return V3Chat(DeviceKeysSession(user, ::legacy, storage, engine, outgoing), user, File(folder.root, "$name-history"), outgoing)
    }

    private fun V3Chat.one(conversation: String, id: String) = shown(conversation).first { it.record.id == id }

    @Test fun `a reply points at what it answers on the other phone, with its preview`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val question = (a.send(chat, false, bob, "lunch?") as SendOutcome.Sent).record
        b.receive()
        val reply = MessageExtras(reply = chatReply(question.id, "lunch?"))
        val answer = (b.send(chat, false, alice, "yes", extras = reply) as SendOutcome.Sent).record
        assertEquals(question.id, answer.reply?.target)

        assertEquals(listOf("yes"), a.receive()!!.received.map { it.text })
        val shown = a.one(chat, answer.id)
        assertEquals(question.id, shown.replyTo)
        assertEquals("lunch?", shown.replyPreview)
        assertFalse(shown.forwarded)
    }

    @Test fun `a group poll counts one vote from each of two members, and votes after its end don't count`() = runBlocking {
        server.participants[group] = listOf(alice, bob, carol)
        val a = account(alice, "a")
        val b = account(bob, "b")
        val c = account(carol, "c")
        val poll = (a.sendPoll(group, true, "", "Lunch?", listOf("Pizza", "Sushi"), multi = false) as SendOutcome.Sent).record
        assertEquals(listOf("Lunch?"), b.receive()!!.received.map { it.text })
        c.receive()
        assertEquals(listOf("Pizza", "Sushi"), b.one(group, poll.id).poll?.options)

        b.sendOp(group, true, "", ChatOp.vote(poll.id, listOf(1)))
        c.sendOp(group, true, "", ChatOp.vote(poll.id, listOf(0)))
        // A vote for two options on a single-choice poll: skipped, Carol's first vote stands.
        delay(5)
        c.sendOp(group, true, "", ChatOp.vote(poll.id, listOf(0, 1)))
        a.receive()
        assertEquals(listOf(1, 1), a.one(group, poll.id).poll?.votes)
        assertEquals(mapOf(bob to listOf(1), carol to listOf(0)), a.one(group, poll.id).poll?.choices)

        delay(5)
        a.sendOp(group, true, "", ChatOp.end(poll.id))
        delay(5)
        b.receive()
        b.sendOp(group, true, "", ChatOp.vote(poll.id, listOf(0)))
        a.receive()
        c.receive()
        for (phone in listOf(a, c)) {
            val folded = phone.one(group, poll.id).poll!!
            assertTrue(folded.ended)
            assertEquals(listOf(1, 1), folded.votes)
        }
    }

    @Test fun `anyone in the chat pins or unpins, and the other phone follows`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val plan = (a.send(chat, false, bob, "the plan") as SendOutcome.Sent).record
        b.receive()
        b.sendOp(chat, false, alice, ChatOp.pin(plan.id, true))
        a.receive()
        assertTrue(a.one(chat, plan.id).pinned)
        assertEquals(bob, a.one(chat, plan.id).pinnedBy)
        delay(5)
        a.sendOp(chat, false, bob, ChatOp.pin(plan.id, false))
        b.receive()
        assertFalse(b.one(chat, plan.id).pinned)
    }

    @Test fun `receipts go back to the sender, delivered on store, read once on screen, never twice`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val sent = (a.send(chat, false, bob, "hi") as SendOutcome.Sent).record
        b.receive()
        val t = System.currentTimeMillis()
        assertEquals(1, b.sendReceipts(chat, now = t))
        // Nothing new owed, and at most once every 2 s anyway.
        assertEquals(0, b.sendReceipts(chat, now = t + 3_000))
        a.receive()
        assertEquals(listOf(ShownReceipt(bob, ChatOp.DELIVERED)), a.one(chat, sent.id).receipts)

        assertEquals(1, b.sendReceipts(chat, onScreen = listOf(sent.id), now = t + 6_000))
        assertEquals(0, b.sendReceipts(chat, onScreen = listOf(sent.id), now = t + 9_000))
        a.receive()
        assertEquals(listOf(ShownReceipt(bob, ChatOp.READ)), a.one(chat, sent.id).receipts)
        // No receipts for receipts.
        assertEquals(0, a.sendReceipts(chat, now = t + 12_000))
    }

    @Test fun `in a group a receipt goes to the message's sender only, and each member's shows`() = runBlocking {
        server.participants[group] = listOf(alice, bob, carol)
        val a = account(alice, "a")
        val b = account(bob, "b")
        val c = account(carol, "c")
        val sent = (a.send(group, true, "", "hello all") as SendOutcome.Sent).record
        b.receive()
        c.receive()
        assertEquals(1, b.sendReceipts(group))
        assertEquals(1, c.sendReceipts(group, onScreen = listOf(sent.id)))
        a.receive()
        assertEquals(listOf(ShownReceipt(bob, ChatOp.DELIVERED), ShownReceipt(carol, ChatOp.READ)), a.one(group, sent.id).receipts)
        // Carol never got Bob's receipt.
        assertTrue(c.receive()!!.received.none { it.op?.t == ChatOp.RECEIPT })
    }

    @Test fun `a disappearing message is erased from the phone when its time comes`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val sent = (a.send(chat, false, bob, "soon gone", extras = MessageExtras(exp = 60)) as SendOutcome.Sent).record
        b.receive()
        val stored = b.records(chat).single()
        assertEquals(60L, stored.exp)
        assertEquals(stored.ts + 60_000, b.one(chat, sent.id).expiresAt)

        val later = stored.ts + 60_000
        assertTrue(b.shown(chat, now = later).isEmpty())
        assertEquals("", b.records(chat).single().text)
        assertTrue(a.shown(chat, now = later).isEmpty())
        assertEquals("", a.records(chat).single().text)
    }

    private val upload = FileDescriptor(
        "file", 1, "33333333-3333-4333-8333-333333333333", "c2VjcmV0LWZpbGUta2V5LXNlY3JldC1maWxlLWtleSE=", "notes.txt", "text/plain", 5, 46, "a".repeat(64),
        FileEncryptionInfo("xchacha20poly1305-secretstream", 1, "aGVhZGVyaGVhZGVyaGVhZGVyaGVhZGVy", 65536),
    )
    private val copy = upload.copy(objectId = "44444444-4444-4444-8444-444444444444", fileKey = "bmV3LWtleS1uZXcta2V5LW5ldy1rZXktbmV3LWtleSE=")

    @Test fun `a forwarded file goes out uploaded again under a new key, marked forwarded`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val deleted = mutableListOf<String>()
        val outcome = a.forwardFile(chat, false, bob, upload, reupload = { copy }, deleteUpload = { deleted += it })
        assertTrue(outcome is SendOutcome.Sent)
        val got = b.receive()!!.received.single()
        assertEquals(copy.objectId, got.file?.objectId)
        assertEquals(copy.fileKey, got.file?.fileKey)
        assertTrue(got.forwarded)
        assertTrue(b.one(chat, got.id).forwarded)

        // The same descriptor again is refused before anything is sent.
        val reused = runCatching { a.forwardFile(chat, false, bob, upload, reupload = { upload }, deleteUpload = { deleted += it }) }.exceptionOrNull()
        assertEquals("file_reused", (reused as EngineException).code)
        assertTrue(b.receive()!!.received.isEmpty())
        assertTrue(deleted.isEmpty())
    }

    @Test fun `with Tor down, replies, polls, votes, pins and receipts wait in the queue and arrive once it's back`() = runBlocking {
        val tor = TorDropsPosts(server.api(alice))
        val a = queuedAccount(alice, "a", tor)
        val b = account(bob, "b")
        val first = (b.send(chat, false, alice, "first") as SendOutcome.Sent).record
        a.receive()

        val reply = a.send(chat, false, bob, "answer", extras = MessageExtras(reply = chatReply(first.id, "first"), exp = 3600)) as SendOutcome.Sent
        assertEquals(V3Chat.QUEUED, reply.record.status)
        val poll = a.sendPoll(chat, false, bob, "Lunch?", listOf("A", "B"), multi = true) as SendOutcome.Sent
        assertEquals(V3Chat.QUEUED, poll.record.status)
        a.sendOp(chat, false, bob, ChatOp.vote(poll.record.id, listOf(0, 1)))
        a.sendOp(chat, false, bob, ChatOp.pin(first.id, true))
        assertEquals(1, a.sendReceipts(chat, onScreen = listOf(first.id)))
        assertEquals(5, a.queuedMessages())
        assertTrue(b.receive()!!.received.isEmpty())
        assertFalse(a.flushQueue())

        tor.down = false
        assertTrue(a.flushQueue())
        assertEquals(0, a.queuedMessages())
        assertEquals(listOf("sent", "sent"), a.records(chat).filter { it.direction == "out" && it.op == null }.map { it.status })
        b.receive()
        val answer = b.one(chat, reply.record.id)
        assertEquals(first.id, answer.replyTo)
        assertEquals(answer.record.ts + 3_600_000, answer.expiresAt)
        assertEquals(listOf(1, 1), b.one(chat, poll.record.id).poll?.votes)
        val mine = b.one(chat, first.id)
        assertTrue(mine.pinned)
        assertEquals(listOf(ShownReceipt(alice, ChatOp.READ)), mine.receipts)
        assertNull(b.failed(first.id))
    }
}
