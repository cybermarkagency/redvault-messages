// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom
import java.util.concurrent.TimeUnit

/**
 * Gap 1 of docs/signal-integration/13-every-device-and-session-reset.md on Android: which sends say `allDevices`, how a
 * marked send is split (never one account across two requests), and that the field goes only to a server whose
 * /e2e/v3/config has `allDevices: true`. The engine-backed tests are skipped when the engine library is missing.
 */
class AllDevicesTest {
    @get:Rule val folder = TemporaryFolder()

    // --- Chunks by account ---

    private fun copies(accounts: Int, devices: Int, prefix: String = "u") =
        (1..accounts).flatMap { a -> (1..devices).map { d -> Triple("$prefix$a", "d$d", "body") } }

    @Test fun `a marked send over 100 devices is split by account, never cutting one across two requests`() {
        val items = copies(15, 7) // 105 copies
        val chunks = chunkByAccount(items, { it.first })
        assertEquals(listOf(98, 7), chunks.map { it.size })
        assertTrue(chunks.all { it.size <= ChatLimits.MAX_RECIPIENTS })
        val owners = chunks.map { c -> c.map { it.first }.toSet() }
        assertTrue("no account in two requests", owners[0].intersect(owners[1]).isEmpty())
        assertEquals(items.toSet(), chunks.flatten().toSet())
        // Unmarked sends keep the plain 100-copy chunks.
        assertEquals(listOf(100, 5), sendChunks(items, false, { it.first }).map { it.size })
        assertEquals(listOf(98, 7), sendChunks(items, true, { it.first }).map { it.size })
    }

    @Test fun `own devices appended last stay together in the last request`() {
        val items = copies(14, 7) + copies(1, 9, prefix = "me") // 98 + 9
        val chunks = chunkByAccount(items, { it.first })
        assertEquals(listOf(98, 9), chunks.map { it.size })
        assertTrue(chunks.last().all { it.first == "me1" })
    }

    @Test fun `accounts keep their first-seen order and devices stay together even when interleaved`() {
        val items = listOf(Triple("a", "1", ""), Triple("b", "1", ""), Triple("a", "2", ""))
        assertEquals(listOf(listOf(Triple("a", "1", ""), Triple("a", "2", ""), Triple("b", "1", ""))), chunkByAccount(items, { it.first }))
        assertTrue(chunkByAccount(emptyList<Triple<String, String, String>>(), { it.first }).isEmpty())
    }

    @Test fun `postInBatches by account never cuts an account`() = runBlocking {
        val seen = mutableListOf<List<Triple<String, String, String>>>()
        postInBatches(copies(15, 7), { it.first }, byAccount = true) { seen += it }
        assertEquals(listOf(98, 7), seen.map { it.size })
        seen.clear()
        postInBatches(copies(15, 7), { it.first }) { seen += it }
        assertEquals(listOf(100, 5), seen.map { it.size })
    }

    // --- Config gating (RetrofitV3Api) ---

    private val web = MockWebServer().apply { start() }

    @After fun stop() = web.shutdown()

    private fun api() = RetrofitV3Api(ApiClient.createV3(web.url("/api/").toString(), OkHttpClient()))

    private fun body(request: okhttp3.mockwebserver.RecordedRequest): JsonObject = Json.parseToJsonElement(request.body.readUtf8()).jsonObject

    @Test fun `a server whose config lacks allDevices never gets the field`() = runBlocking {
        web.enqueue(MockResponse().setBody("""{"enabled":true,"signal":true}"""))
        web.enqueue(MockResponse().setBody("{}"))
        web.enqueue(MockResponse().setBody("{}"))
        val api = api()
        api.sendMessages("device-1", "conv", listOf(Triple("user-2", "device-2", "ct")), allDevices = true)
        api.sendGroupMessage("device-1", "conv", "gct", listOf("user-2" to "device-2"), allDevices = true)
        assertEquals("/api/e2e/v3/config", web.takeRequest(5, TimeUnit.SECONDS)!!.path)
        val direct = web.takeRequest(5, TimeUnit.SECONDS)!!
        assertEquals("/api/e2e/v3/messages", direct.path)
        assertFalse(body(direct).containsKey("allDevices"))
        val group = web.takeRequest(5, TimeUnit.SECONDS)!!
        assertEquals("/api/e2e/v3/group-messages", group.path)
        assertFalse(body(group).containsKey("allDevices"))
        assertEquals("the config is read once", 3, web.requestCount)
    }

    @Test fun `a server whose config says allDevices gets it on marked sends only`() = runBlocking {
        web.enqueue(MockResponse().setBody("""{"enabled":true,"allDevices":true}"""))
        repeat(3) { web.enqueue(MockResponse().setBody("{}")) }
        val api = api()
        assertTrue(api.enabled())
        api.sendMessages("device-1", "conv", listOf(Triple("user-2", "device-2", "ct")), allDevices = true)
        api.sendGroupMessage("device-1", "conv", "gct", listOf("user-2" to "device-2"), allDevices = true)
        api.sendMessages("device-1", "conv", listOf(Triple("user-2", "device-2", "ct")))
        assertEquals("/api/e2e/v3/config", web.takeRequest(5, TimeUnit.SECONDS)!!.path)
        assertEquals("true", body(web.takeRequest(5, TimeUnit.SECONDS)!!)["allDevices"].toString())
        assertEquals("true", body(web.takeRequest(5, TimeUnit.SECONDS)!!)["allDevices"].toString())
        assertFalse(body(web.takeRequest(5, TimeUnit.SECONDS)!!).containsKey("allDevices"))
        assertEquals(4, web.requestCount)
    }

    @Test fun `an unmarked send never reads the config, and a config that can't be read counts as an older server`() = runBlocking {
        web.enqueue(MockResponse().setBody("{}"))
        web.enqueue(MockResponse().setResponseCode(500).setBody("""{"error":"internal"}"""))
        web.enqueue(MockResponse().setBody("{}"))
        val api = api()
        api.sendMessages("device-1", null, listOf(Triple("user-2", "device-2", "ct")))
        assertEquals("/api/e2e/v3/messages", web.takeRequest(5, TimeUnit.SECONDS)!!.path)
        api.sendMessages("device-1", null, listOf(Triple("user-2", "device-2", "ct")), allDevices = true)
        assertEquals("/api/e2e/v3/config", web.takeRequest(5, TimeUnit.SECONDS)!!.path)
        assertFalse(body(web.takeRequest(5, TimeUnit.SECONDS)!!).containsKey("allDevices"))
    }

    // --- Which sends are marked (real engine, in-memory server that enforces the rule) ---

    private val server = FakeV3Server()
    private val alice = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val bob = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    private val chat = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"
    private val group = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"

    private fun engine(signal: Boolean = false): EngineRunner {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        val engine = EngineRunner(call!!)
        if (signal) assumeTrue("this engine has no Signal", engine.run(request("version")).json["signal"] is JsonObject)
        server.participants[chat] = listOf(alice, bob)
        server.participants[group] = listOf(alice, bob)
        return engine
    }

    private fun legacy() = ByteArray(32).also { SecureRandom().nextBytes(it) }
    private fun storage(name: String) = DeviceStorage(File(folder.root, name), SoftwareKeyWrapper())
    private fun history(name: String, device: V3Device) = EncryptedChatHistory(File(folder.root, "$name.hist"), device.historyKey())

    private class Devices(val a1: V3Device, val a2: V3Device, val b1: V3Device, val b2: V3Device)

    private suspend fun devices(engine: EngineRunner, signal: Boolean): Devices {
        val a1 = setUpFirstDevice(alice, legacy(), storage("a1"), engine, server.api(alice))
        val a2 = restoreWithRecoveryKey(alice, createRecoveryBackup(a1), storage("a2"), engine, server.api(alice))
        val b1 = setUpFirstDevice(bob, legacy(), storage("b1"), engine, server.api(bob))
        val b2 = restoreWithRecoveryKey(bob, createRecoveryBackup(b1), storage("b2"), engine, server.api(bob))
        if (signal) {
            for (d in listOf(a1, a2, b1, b2)) assertEquals("enabled", maintainSignal(d))
            maintainSignal(a1)
            maintainSignal(b1)
        }
        return Devices(a1, a2, b1, b2)
    }

    private fun since(start: Int) = server.requests.drop(start)

    @Test fun `one-to-one messages, ops and receipts are marked and reach every device`() = runBlocking {
        val d = devices(engine(), signal = false)
        val ha = history("a1", d.a1)
        var start = server.requests.size
        assertTrue(sendText(d.a1, ha, chat, bob, "hello") is SendOutcome.Sent)
        val sent = since(start).single()
        assertTrue(sent.allDevices)
        assertEquals(setOf(bob to d.b1.deviceId, bob to d.b2.deviceId, alice to d.a2.deviceId), sent.recipients.toSet())

        start = server.requests.size
        val hb = history("b1", d.b1)
        val id = receiveMessages(d.b1, hb).received.single().id
        sendChatOp(d.b1, hb, chat, false, alice, ChatOp.reaction(id, "+"))
        sendChatReceipt(d.b1, hb, chat, alice, listOf(id), "read")
        assertTrue(since(start).isNotEmpty() && since(start).all { it.allDevices })
    }

    @Test fun `a Megolm group marks the fan-out, never the room-key shares`() = runBlocking {
        val d = devices(engine(), signal = false)
        val start = server.requests.size
        assertTrue(sendGroupText(d.a1, history("a1", d.a1), group, "hi group") is SendOutcome.Sent)
        val requests = since(start)
        val keys = requests.filter { it.kind == "direct" }
        assertTrue("room keys were shared", keys.isNotEmpty())
        assertTrue(keys.none { it.allDevices })
        val fanOut = requests.single { it.kind == "group" }
        assertTrue(fanOut.allDevices)
        assertEquals(3, fanOut.recipients.size)
    }

    @Test fun `a Signal group marks the fan-out, never the sender-key distribution`() = runBlocking {
        val d = devices(engine(signal = true), signal = true)
        val start = server.requests.size
        assertTrue(sendGroupText(d.a1, history("a1", d.a1), group, "hi signal group") is SendOutcome.Sent)
        val requests = since(start)
        val keys = requests.filter { it.kind == "direct" }
        assertTrue("sender keys were shared", keys.isNotEmpty())
        assertTrue(keys.none { it.allDevices })
        assertTrue(requests.single { it.kind == "group" }.allDevices)
    }

    @Test fun `a call offer to every device and group call keys are marked, an answer to one device is not`() = runBlocking {
        val d = devices(engine(), signal = false)
        val callId = "11111111-1111-4111-8111-111111111111"
        var start = server.requests.size
        sendCallMessage(d.a1, chat, bob, CallMessage(callId, CallMessage.OFFER, "audio", "v=0\r\na=fingerprint:sha-256 AA\r\n", CallMessage.EVERY_DEVICE))
        val offer = since(start).single()
        assertTrue(offer.allDevices)
        assertEquals(setOf(bob to d.b1.deviceId, bob to d.b2.deviceId), offer.recipients.toSet())

        start = server.requests.size
        sendCallMessage(d.b1, chat, alice, CallMessage(callId, CallMessage.ANSWER, "audio", "v=0\r\nanswer\r\n", d.a1.deviceId), onlyDevice = d.a1.deviceId)
        val answer = since(start).single()
        assertFalse(answer.allDevices)
        assertEquals(listOf(alice to d.a1.deviceId), answer.recipients)

        start = server.requests.size
        sendCallKeyMessage(d.a1, group, listOf(alice, bob), CallKeyMessage(callId, 1, ByteArray(32) { 7 }))
        assertTrue(since(start).single().allDevices)
    }
}
