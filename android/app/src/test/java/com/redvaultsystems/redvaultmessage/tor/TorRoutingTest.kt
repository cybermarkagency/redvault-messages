// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.Request
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.DataInputStream
import java.io.IOException
import java.net.Authenticator
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Proxy
import java.net.ServerSocket
import java.net.Socket
import java.net.URI
import java.net.UnknownHostException
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

/**
 * The OkHttp side of Tor routing (tor/TorRouting.kt), on the JVM's own
 * java.net SOCKS5 client (the same OpenJDK code Android runs), against a
 * stand-in for tor-core's loopback gate: requests reach it by name with the
 * gate's credentials, nothing is looked up, and nothing goes out while Tor
 * is down or to any host but an onion service.
 */
class TorRoutingTest {
    private val onion = "a".repeat(52) + "aaad.onion"
    private val gate = FakeGate(username = "u" + "1".repeat(31), password = "p" + "2".repeat(31))
    @Volatile private var up = true
    private val source = TorGateSource { if (up) SocksEndpoint("127.0.0.1", gate.port, gate.username, gate.password) else null }

    @After
    fun tearDown() = gate.close()

    private fun client(): OkHttpClient = TorRouting.install(OkHttpClient.Builder(), source).build()

    @Test
    fun onionRequestsGoThroughTheGateByNameWithItsCredentials() {
        client().newCall(Request.Builder().url("http://$onion/api/health").build()).execute().use { response ->
            assertEquals(200, response.code)
            assertEquals("ok", response.body!!.string())
        }
        val seen = gate.seen.single()
        assertEquals(gate.username, seen.username)
        assertEquals(gate.password, seen.password)
        // ATYP 3: the name itself, never an address the phone looked up.
        assertEquals(3, seen.addressType)
        assertEquals(onion, seen.host)
        assertEquals(80, seen.port)
        assertTrue(seen.httpRequest.startsWith("GET /api/health HTTP/1.1"))
    }

    @Test
    fun nothingGoesOutWhileTorIsDown() {
        up = false
        try {
            client().newCall(Request.Builder().url("http://$onion/api/health").build()).execute().close()
            fail("expected TorUnavailableException")
        } catch (e: TorUnavailableException) {
            // expected
        }
        assertEquals(0, gate.connections.get())
    }

    @Test
    fun onlyTheOnionServiceIsReachable() {
        for (url in listOf("http://example.com/", "https://www.redvaultsystems.com/redvaultmessages/api/health", "http://127.0.0.1:${gate.port}/")) {
            try {
                client().newCall(Request.Builder().url(url).build()).execute().close()
                fail("expected a refusal for $url")
            } catch (e: IOException) {
                // expected
            }
        }
        assertEquals(0, gate.connections.get())
    }

    @Test
    fun theProxyListIsNeverEmptySoOkHttpCanNeverGoDirect() {
        up = false
        val proxies = TorRouting.GateSelector(source).select(URI("http://$onion/"))
        assertEquals(1, proxies.size)
        assertEquals(Proxy.Type.SOCKS, proxies[0].type())
        val address = proxies[0].address() as InetSocketAddress
        assertTrue(address.address.isLoopbackAddress)
        assertEquals(9, address.port)
    }

    @Test(expected = UnknownHostException::class)
    fun dnsLookupsFail() {
        TorRouting.NoDns.lookup(onion)
    }

    @Test
    fun theCredentialsAreGivenOnlyForTheGatesOwnSocksRequest() {
        TorRouting.SocksCredentials.install(source)
        val loopback = InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1))
        val ok = Authenticator.requestPasswordAuthentication("127.0.0.1", loopback, gate.port, "SOCKS5", "SOCKS authentication", null)
        assertEquals(gate.username, ok!!.userName)
        assertNull(Authenticator.requestPasswordAuthentication("127.0.0.1", loopback, gate.port + 1, "SOCKS5", "SOCKS authentication", null))
        assertNull(Authenticator.requestPasswordAuthentication("127.0.0.1", loopback, gate.port, "http", "Basic", "basic"))
        assertNull(Authenticator.requestPasswordAuthentication("example.com", null, gate.port, "SOCKS5", "SOCKS authentication", null))
        up = false
        assertNull(Authenticator.requestPasswordAuthentication("127.0.0.1", loopback, gate.port, "SOCKS5", "SOCKS authentication", null))
    }

    /** A minimal SOCKS5 gate (username/password, CONNECT) that answers one HTTP request itself. */
    // --- The background connection (push/PushFallbackService.kt) in a Tor build ---
    // Strict no-push mode still allows it with Tor routing (push/PushRegistrar.kt), on the promise that it
    // never leaves except through the Tor gate. The push client is derived from the app's Tor-routed client
    // (ApiClient.createPush), so it has to keep that routing.

    @Test
    fun pushWaitGoesOnlyThroughTheGateByName() {
        val push = ApiClient.createPush("http://$onion/api/", client())
        // The stand-in gate answers "ok", not JSON: only where the request went matters here.
        runCatching { runBlocking { push.wait() } }
        val seen = gate.seen.single()
        assertEquals(3, seen.addressType)
        assertEquals(onion, seen.host)
        assertTrue(seen.httpRequest.startsWith("GET /api/push/wait HTTP/1.1"))
    }

    @Test
    fun pushWaitSendsNothingWhileTorIsDown() {
        up = false
        val push = ApiClient.createPush("http://$onion/api/", client())
        val error = runCatching { runBlocking { push.wait(after = 7) } }.exceptionOrNull()
        assertTrue("expected TorUnavailableException, got $error", error is TorUnavailableException)
        assertTrue(gate.seen.isEmpty())
    }

    private class FakeGate(val username: String, val password: String) : AutoCloseable {
        class Seen(val username: String, val password: String, val addressType: Int, val host: String, val port: Int, val httpRequest: String)

        private val server = ServerSocket(0, 50, InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1)))
        val port: Int get() = server.localPort
        val seen = CopyOnWriteArrayList<Seen>()
        val connections = AtomicInteger()

        init {
            thread(isDaemon = true) {
                while (!server.isClosed) {
                    val socket = runCatching { server.accept() }.getOrNull() ?: break
                    connections.incrementAndGet()
                    thread(isDaemon = true) { runCatching { socket.use(::handle) } }
                }
            }
        }

        private fun handle(socket: Socket) {
            val input = DataInputStream(socket.getInputStream())
            val output = socket.getOutputStream()
            check(input.readUnsignedByte() == 5)
            val methods = ByteArray(input.readUnsignedByte()).also(input::readFully)
            check(methods.contains(2.toByte())) { "the client didn't offer username/password" }
            output.write(byteArrayOf(5, 2))
            check(input.readUnsignedByte() == 1)
            val user = String(ByteArray(input.readUnsignedByte()).also(input::readFully))
            val pass = String(ByteArray(input.readUnsignedByte()).also(input::readFully))
            if (user != username || pass != password) {
                output.write(byteArrayOf(1, 1))
                return
            }
            output.write(byteArrayOf(1, 0))
            check(input.readUnsignedByte() == 5)
            check(input.readUnsignedByte() == 1) // CONNECT
            input.readUnsignedByte()
            val type = input.readUnsignedByte()
            val host = when (type) {
                3 -> String(ByteArray(input.readUnsignedByte()).also(input::readFully))
                1 -> InetAddress.getByAddress(ByteArray(4).also(input::readFully)).hostAddress
                else -> InetAddress.getByAddress(ByteArray(16).also(input::readFully)).hostAddress
            }
            val port = input.readUnsignedShort()
            output.write(byteArrayOf(5, 0, 0, 1, 0, 0, 0, 0, 0, 0))
            val request = StringBuilder()
            while (!request.endsWith("\r\n\r\n")) request.append(input.readUnsignedByte().toChar())
            seen += Seen(user, pass, type, host, port, request.toString())
            output.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok".toByteArray())
            output.flush()
        }

        override fun close() = server.close()
    }
}
