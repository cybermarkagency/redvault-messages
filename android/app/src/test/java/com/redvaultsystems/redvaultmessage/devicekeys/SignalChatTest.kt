// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom

/**
 * rv-signal-v1 on Android (docs/signal-integration/), on the JVM: the real engine built with the
 * `signal` feature (-PredvaultE2eSignal=true), and an in-memory server. Skipped when the engine
 * library is missing or has no Signal.
 */
class SignalChatTest {
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
        val version = engine.run(request("version"))
        assumeTrue("this engine has no Signal (build with -PredvaultE2eSignal=true)", version.json["signal"] is JsonObject)
        server.participants[chat] = listOf(alice, bob)
    }

    private fun legacy() = ByteArray(32).also { SecureRandom().nextBytes(it) }
    private fun storage(name: String) = DeviceStorage(File(folder.root, name), SoftwareKeyWrapper())
    private fun history(name: String, device: V3Device) = EncryptedChatHistory(File(folder.root, "$name.hist"), device.historyKey())
    private suspend fun first(user: String, name: String) = setUpFirstDevice(user, legacy(), storage(name), engine, server.api(user))
    private suspend fun second(user: String, name: String, existing: V3Device): V3Device =
        restoreWithRecoveryKey(user, createRecoveryBackup(existing), storage(name), engine, server.api(user))

    /** The mailbox bodies waiting for a device, as the server holds them. */
    private fun bodies(user: String, device: String): List<JsonObject> =
        server.mailbox.filter { it.toUser == user && it.toDevice == device }.map { Json.parseToJsonElement(it.item["body"]!!.jsonPrimitive.content).jsonObject }

    @Test fun `devices turn Signal on, declare it in the signed list and keep keys topped up`() = runBlocking {
        val a1 = first(alice, "a1")
        assertEquals("enabled", maintainSignal(a1))
        assertEquals(20, server.signal["$alice/${a1.deviceId}"]!!.preKeys.size)
        val caps = server.api(alice).deviceList(alice).deviceList!!["capabilities"]!!.jsonArray
        assertEquals(a1.deviceId, caps.single().jsonObject["deviceId"]!!.jsonPrimitive.content)
        assertEquals("ok", maintainSignal(a1))
        repeat(18) { server.signal["$alice/${a1.deviceId}"]!!.preKeys.removeFirst() }
        maintainSignal(a1)
        assertEquals(22, server.signal["$alice/${a1.deviceId}"]!!.preKeys.size)
        server.signalOn = false
        assertEquals("off", maintainSignal(a1))
    }

    @Test fun `one-to-one chat with own-device copies goes over rv-signal-v1`() = runBlocking {
        val a1 = first(alice, "a1")
        val a2 = second(alice, "a2", a1)
        val b1 = first(bob, "b1")
        for (d in listOf(a1, a2, b1)) maintainSignal(d)
        // a2 joined through the recovery key and declared itself; a1 must see the latest own list before sending.
        maintainSignal(a1)
        val ha1 = history("a1", a1)
        val hb1 = history("b1", b1)
        val ha2 = history("a2", a2)

        assertTrue(sendText(a1, ha1, chat, bob, "hello over signal") is SendOutcome.Sent)
        for ((user, device) in listOf(bob to b1.deviceId, alice to a2.deviceId)) {
            val body = bodies(user, device).single()
            assertEquals("rv-signal-v1", body["protocol"]!!.jsonPrimitive.content)
            assertEquals("prekey", body["type"]!!.jsonPrimitive.content)
            assertEquals(a1.deviceId, body["certificate"]!!.jsonObject["deviceId"]!!.jsonPrimitive.content)
        }
        assertTrue(server.sent.none { it.contains("hello over signal") })
        assertEquals(listOf("hello over signal"), receiveMessages(b1, hb1).received.map { it.text })
        assertEquals("out", receiveMessages(a2, ha2).received.single().direction)

        sendText(b1, hb1, chat, alice, "hi alice")
        assertEquals("rv-signal-v1", bodies(alice, a1.deviceId).single()["protocol"]!!.jsonPrimitive.content)
        assertEquals(listOf("hi alice"), receiveMessages(a1, ha1).received.map { it.text })

        // The engine itself refuses Olm chat between the two.
        val olm = a1.op("encrypt", "recipient" to b1.identity(), "plaintext" to "downgrade?")
        assertEquals("downgrade_refused", olm.error)
    }

    @Test fun `a contact without Signal keeps getting Olm, and a hidden Signal device fails the send`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        assertEquals("enabled", maintainSignal(a1))
        val ha1 = history("a1", a1)
        val hb1 = history("b1", b1)

        sendText(a1, ha1, chat, bob, "olm for bob")
        assertTrue(bodies(bob, b1.deviceId).single()["protocol"] == null)
        assertEquals(listOf("olm for bob"), receiveMessages(b1, hb1).received.map { it.text })

        assertEquals("enabled", maintainSignal(b1))
        server.withholdSignal = true
        val error = runCatching { sendText(a1, ha1, chat, bob, "must not go over olm") }.exceptionOrNull()
        assertTrue("$error", error is EngineException && error.code == "signal_keys_missing")
        assertEquals(0, server.pending(bob, b1.deviceId))
        assertTrue(server.sent.none { it.contains("must not go over olm") })
    }

    @Test fun `the chat says which protocol it uses, and turning Signal off moves it back`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        assertEquals("olm", chatProtocol(a1, chat, bob))
        maintainSignal(a1)
        assertEquals("olm", chatProtocol(a1, chat, bob))
        maintainSignal(b1)
        assertEquals("signal", chatProtocol(a1, chat, bob))
        sendText(a1, history("a1", a1), chat, bob, "over signal")
        assertEquals("rv-signal-v1", bodies(bob, b1.deviceId).single()["protocol"]!!.jsonPrimitive.content)
        receiveMessages(b1, history("b1", b1))

        turnOffSignal(b1, b1.deviceId)
        assertEquals("olm", chatProtocol(a1, chat, bob))
        sendText(a1, history("a1", a1), chat, bob, "back on olm")
        assertTrue(bodies(bob, b1.deviceId).single()["protocol"] == null)
        assertEquals(listOf("back on olm"), receiveMessages(b1, history("b1", b1)).received.map { it.text })
    }

    @Test fun `the thread note names the protocol and waiting messages`() {
        assertTrue(com.redvaultsystems.redvaultmessage.ui.deviceKeysNote("signal", 0)!!.startsWith("Signal sessions"))
        assertTrue(com.redvaultsystems.redvaultmessage.ui.deviceKeysNote("mixed", 2)!!.contains("2 device-key messages are waiting"))
        assertEquals(null, com.redvaultsystems.redvaultmessage.ui.deviceKeysNote("olm", 0))
    }

    @Test fun `a group goes over sender keys, and a removed member reads nothing new`() = runBlocking {
        val carol = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
        val group = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val c1 = first(carol, "c1")
        for (d in listOf(a1, b1, c1)) assertEquals("enabled", maintainSignal(d))
        server.participants[group] = listOf(alice, bob, carol)
        val ha = history("a1", a1)
        val hb = history("b1", b1)
        val hc = history("c1", c1)

        assertTrue(sendGroupText(a1, ha, group, "hello group") is SendOutcome.Sent)
        assertEquals(1, server.rosters[group]!!.size)
        val atBob = bodies(bob, b1.deviceId)
        assertEquals(listOf("rv-signal-v1", "rv-signal-group-v1"), atBob.map { it["protocol"]!!.jsonPrimitive.content })
        assertTrue(server.sent.none { it.contains("hello group") })
        assertEquals(listOf("hello group"), receiveMessages(b1, hb).received.map { it.text })
        assertEquals(listOf("hello group"), receiveMessages(c1, hc).received.map { it.text })

        sendGroupText(b1, hb, group, "hi from bob")
        assertEquals(listOf("hi from bob"), receiveMessages(a1, ha).received.map { it.text })
        assertEquals(listOf("hi from bob"), receiveMessages(c1, hc).received.map { it.text })
        assertEquals(0, server.pending(carol, c1.deviceId))

        // carol leaves: the next send signs epoch 2 without her, under a new sender key she never gets.
        server.participants[group] = listOf(alice, bob)
        sendGroupText(a1, ha, group, "after carol left")
        assertEquals(2, server.rosters[group]!!.size)
        assertEquals(listOf("after carol left"), receiveMessages(b1, hb).received.map { it.text })
        assertEquals(0, server.pending(carol, c1.deviceId))
    }
}
