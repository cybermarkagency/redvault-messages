// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom

/**
 * Gap 2 of docs/signal-integration/13-every-device-and-session-reset.md on Android: a device whose Signal session with
 * a contact fell out of step (here: its state rolled back to an older copy) resets it and sends one refresh, and the
 * chat works again. Real engine with the `signal` feature, in-memory server; skipped without them.
 */
class SessionResetTest {
    @get:Rule val folder = TemporaryFolder()

    private lateinit var engine: EngineRunner
    private val server = FakeV3Server()
    private val alice = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val bob = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    private val chat = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"

    @Before fun load() {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        engine = EngineRunner(call!!)
        assumeTrue("this engine has no Signal", engine.run(request("version")).json["signal"] is JsonObject)
        server.participants[chat] = listOf(alice, bob)
    }

    private fun legacy() = ByteArray(32).also { SecureRandom().nextBytes(it) }
    private fun storage(name: String) = DeviceStorage(File(folder.root, name), SoftwareKeyWrapper())
    private fun history(name: String, device: V3Device) = EncryptedChatHistory(File(folder.root, "$name.hist"), device.historyKey())

    private fun bodies(user: String, device: String): List<JsonObject> =
        server.mailbox.filter { it.toUser == user && it.toDevice == device }.map { Json.parseToJsonElement(it.item["body"]!!.jsonPrimitive.content).jsonObject }

    private class Pair2(val a1: V3Device, val b1: V3Device, val ha: ChatHistory, val hb: ChatHistory)

    /** alice and bob on Signal, having talked both ways; bob's device uses [bobApi]. Returns bob's state from back then too. */
    private suspend fun talking(bobApi: V3Api = server.api(bob)): kotlin.Pair<Pair2, String> {
        val a1 = setUpFirstDevice(alice, legacy(), storage("a1"), engine, server.api(alice))
        val b1 = setUpFirstDevice(bob, legacy(), storage("b1"), engine, bobApi)
        assertEquals("enabled", maintainSignal(a1))
        assertEquals("enabled", maintainSignal(b1))
        val p = Pair2(a1, b1, history("a1", a1), history("b1", b1))
        sendText(a1, p.ha, chat, bob, "hello")
        assertEquals(listOf("hello"), receiveMessages(b1, p.hb).received.map { it.text })
        sendText(b1, p.hb, chat, alice, "hi")
        assertEquals(listOf("hi"), receiveMessages(a1, p.ha).received.map { it.text })
        val old = b1.snapshot()
        // The two move on; then bob's phone comes back from the older copy of its state.
        for (round in 0 until 3) {
            sendText(a1, p.ha, chat, bob, "a$round")
            assertEquals(1, receiveMessages(b1, p.hb).received.size)
            sendText(b1, p.hb, chat, alice, "b$round")
            assertEquals(1, receiveMessages(a1, p.ha).received.size)
        }
        return p to old
    }

    @Test fun `a device that can't open a contact's messages resets the session once, and the chat works again`() = runBlocking {
        val (p, old) = talking()
        p.b1.rollback(old)

        sendText(p.a1, p.ha, chat, bob, "can you read this?")
        assertEquals("rv-signal-v1", bodies(bob, p.b1.deviceId).single()["protocol"]!!.jsonPrimitive.content)
        val start = server.requests.size
        val failed = receiveMessages(p.b1, p.hb)
        assertTrue(failed.received.isEmpty())
        assertTrue("${failed.dropped}", failed.dropped.any { it.startsWith("decrypt:") })
        assertEquals("acknowledged as before", 0, server.pending(bob, p.b1.deviceId))

        // One refresh went to alice's device alone, unmarked.
        val refresh = server.requests.drop(start).single()
        assertEquals("direct", refresh.kind)
        assertFalse(refresh.allDevices)
        assertEquals(listOf(alice to p.a1.deviceId), refresh.recipients)
        assertEquals("prekey", bodies(alice, p.a1.deviceId).single()["type"]!!.jsonPrimitive.content)

        // alice opens it and shows nothing.
        val atAlice = receiveMessages(p.a1, p.ha)
        assertTrue(atAlice.received.isEmpty())
        assertEquals(0, server.pending(alice, p.a1.deviceId))

        // From here both ways work again.
        sendText(p.a1, p.ha, chat, bob, "and now?")
        assertEquals(listOf("and now?"), receiveMessages(p.b1, p.hb).received.map { it.text })
        sendText(p.b1, p.hb, chat, alice, "yes")
        assertEquals(listOf("yes"), receiveMessages(p.a1, p.ha).received.map { it.text })
    }

    @Test fun `several failed messages in one pass, or soon after, send one refresh`() = runBlocking {
        val (p, old) = talking()
        p.b1.rollback(old)
        sendText(p.a1, p.ha, chat, bob, "one")
        sendText(p.a1, p.ha, chat, bob, "two")
        val start = server.requests.size
        receiveMessages(p.b1, p.hb)
        assertEquals(1, server.requests.drop(start).size)
        // alice hasn't read the refresh yet, so her next message still fails at bob: within 10 minutes, no second one.
        sendText(p.a1, p.ha, chat, bob, "three")
        val again = server.requests.size
        receiveMessages(p.b1, p.hb)
        assertEquals(again, server.requests.size)
    }

    @Test fun `the refresh never waits in the Tor queue, and a failed one is swallowed`() = runBlocking {
        val queue = V3SendQueue(File(folder.root, "b-queue"), bob, SoftwareKeyWrapper())
        val tor = SendsDown(server.api(bob))
        val (p, old) = talking(QueueingV3Api(tor, queue))
        p.b1.rollback(old)
        sendText(p.a1, p.ha, chat, bob, "lost")
        tor.down = true
        // Even inside a tagged send, the refresh goes straight out (and fails here, quietly).
        val report = withContext(V3SendTag(V3Chat.tagOf(chat, "x"))) { receiveMessages(p.b1, p.hb) }
        assertTrue(report.received.isEmpty())
        assertTrue(queue.isEmpty())
        assertEquals(0, server.pending(alice, p.a1.deviceId))
    }

    @Test fun `messages that fail for other reasons start no reset`() = runBlocking {
        val (p, _) = talking()
        sendText(p.a1, p.ha, chat, bob, "fine")
        // A replayed copy of a message already opened is a duplicate, not a broken session.
        val copy = server.mailbox.last { it.toUser == bob }
        receiveMessages(p.b1, p.hb)
        server.mailbox += FakeV3Server.Mail("replay", bob, p.b1.deviceId, JsonObject(copy.item + ("id" to kotlinx.serialization.json.JsonPrimitive("replay"))))
        val start = server.requests.size
        receiveMessages(p.b1, p.hb)
        assertEquals(start, server.requests.size)
    }

    /** bob's server API with the posts failing like Tor down (everything else works). */
    private class SendsDown(private val inner: V3Api) : V3Api by inner {
        var down = false
        override suspend fun sendMessages(senderDeviceId: String, conversationId: String?, messages: List<Triple<String, String, String>>, allDevices: Boolean) {
            if (down) throw ApiException(0, "tor_unavailable", "Tor connection unavailable")
            inner.sendMessages(senderDeviceId, conversationId, messages, allDevices)
        }
    }
}
