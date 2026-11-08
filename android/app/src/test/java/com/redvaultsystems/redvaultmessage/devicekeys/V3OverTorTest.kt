// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.SessionCookieJar
import com.redvaultsystems.redvaultmessage.tor.ForwardingGate
import com.redvaultsystems.redvaultmessage.tor.ReleaseRouting
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * Device keys and Signal over Tor (docs/tor-by-default.md rule 5): the device-key requests go through the app's
 * Tor-routed client to the real onion host, by name, and nothing goes out while Tor is down; a chat post made then
 * waits in the sealed queue and goes out once Tor is back. The "onion service" is a local MockWebServer behind a
 * stand-in gate ([ForwardingGate]); nothing here reaches the network.
 */
class V3OverTorTest {
    @get:Rule val folder = TemporaryFolder()

    private val server = MockWebServer().apply { start() }
    private val gate = ForwardingGate(server.port)
    private val onionBase = ReleaseRouting.ONION_API_BASE

    @After
    fun tearDown() {
        gate.close()
        server.shutdown()
    }

    /** The device-key API exactly as AppContainer builds it: RetrofitV3Api on the app's Tor-routed client. */
    private fun v3Api(): V3Api = RetrofitV3Api(ApiClient.createV3(onionBase, ApiClient.httpClient(SessionCookieJar(), onionBase, gate.source)))

    @Test
    fun deviceKeyRequestsGoThroughTheGateToTheOnionHostByName() = runBlocking {
        server.enqueue(MockResponse().setBody("{}"))
        server.enqueue(MockResponse().setBody("""{"deviceList":null}"""))
        server.enqueue(MockResponse().setBody("[]"))
        server.enqueue(MockResponse().setBody("{}"))
        val api = v3Api()
        api.sendMessages("device-1", "conv", listOf(Triple("user-2", "device-2", "ciphertext")))
        api.deviceList("user-2", "conv")
        api.signalClaim("conv", listOf("user-2" to "device-2"))
        api.sendGroupMessage("device-1", "conv", "group-ciphertext", listOf("user-2" to "device-2"))

        val paths = (1..4).map { server.takeRequest(5, TimeUnit.SECONDS)!! }.onEach {
            // The onion service sees its own name as the host, nothing else about the phone.
            assertEquals(ReleaseRouting.ONION_HOST, it.getHeader("Host"))
        }.map { "${it.method} ${it.path}" }
        assertEquals(
            listOf(
                "POST /redvaultmessages/api/e2e/v3/messages",
                "GET /redvaultmessages/api/e2e/v3/users/user-2/device-list?conversationId=conv",
                "POST /redvaultmessages/api/e2e/v3/signal/claim",
                "POST /redvaultmessages/api/e2e/v3/group-messages",
            ),
            paths,
        )
        // Every connection asked the gate for the onion name itself (ATYP 3): nothing was looked up on the phone.
        assertTrue(gate.targets.isNotEmpty())
        assertTrue(gate.targets.all { it.addressType == 3 && it.host == ReleaseRouting.ONION_HOST && it.port == 80 })
    }

    @Test
    fun nothingGoesOutWhileTorIsDown() = runBlocking {
        gate.up = false
        val error = runCatching { v3Api().deviceList("user-2", "conv") }.exceptionOrNull()
        assertTrue("expected tor_unavailable, got $error", error is ApiException && error.code == "tor_unavailable")
        assertEquals(0, gate.connections.get())
        assertEquals(0, server.requestCount)
    }

    @Test
    fun aChatPostMadeWhileTorIsDownWaitsSealedAndGoesOutOnceItsBack() = runBlocking {
        val api = QueueingV3Api(v3Api(), V3SendQueue(File(folder.root, "queue"), "user-1", SoftwareKeyWrapper()))
        gate.up = false
        withContext(V3SendTag(V3Chat.tagOf("conv", "m1"))) {
            api.sendMessages("device-1", "conv", listOf(Triple("user-2", "device-2", "ciphertext-1")))
        }
        assertEquals(1, api.queue.size())
        assertEquals(0, gate.connections.get())

        gate.up = true
        server.enqueue(MockResponse().setBody("{}"))
        val drained = api.drain()
        assertTrue(!drained.waiting && V3Chat.tagOf("conv", "m1") in drained.sent)
        val request = server.takeRequest(5, TimeUnit.SECONDS)!!
        assertEquals("/redvaultmessages/api/e2e/v3/messages", request.path)
        assertTrue(request.body.readUtf8().contains("ciphertext-1"))
        assertTrue(gate.targets.all { it.addressType == 3 && it.host == ReleaseRouting.ONION_HOST })
    }
}
