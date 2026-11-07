package com.redvaultsystems.redvaultmessage.privacy

import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import okhttp3.Request
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import java.io.IOException

/**
 * What group-call signaling puts on the wire. LiveKit's Android SDK adds the phone's model, Android
 * version and network type to the connection URL; the app's signaling client removes them (and, like the
 * API client, sends no identifying headers and talks to no host but its own).
 */
class CallSignalingWireTest {
    private lateinit var server: MockWebServer

    @Before
    fun start() {
        server = MockWebServer().apply { start() }
    }

    @After
    fun stop() {
        server.shutdown()
    }

    @Test
    fun theSignalingUrlLosesTheDeviceModelOsAndNetworkType() {
        server.enqueue(MockResponse().setBody("OK"))
        val client = ApiClient.signalingClient(server.hostName)
        val url = server.url(
            "/redvaultmessages/livekit/rtc?protocol=15&auto_subscribe=1&adaptive_stream=1&sdk=android&version=2.28.2" +
                "&device_model=motorola+edge+50+fusion&os=android&os_version=16&network=wifi&client_protocol=1",
        )
        client.newCall(Request.Builder().url(url).build()).execute().use { assertEquals(200, it.code) }
        val seen = server.takeRequest()
        assertEquals(
            "/redvaultmessages/livekit/rtc?protocol=15&auto_subscribe=1&adaptive_stream=1&sdk=android&version=2.28.2&client_protocol=1",
            seen.path,
        )
        // Nor any of the headers the API client already leaves out, or a session cookie.
        for (header in listOf("User-Agent", "Accept-Language", "Referer", "X-Requested-With", "Cookie")) assertNull(header, seen.getHeader(header))
    }

    @Test
    fun theSignalingClientRefusesAnyHostButTheCallServers() {
        val client = ApiClient.signalingClient(server.hostName)
        try {
            client.newCall(Request.Builder().url("http://example.invalid/rtc").build()).execute().close()
            fail("expected the request to be refused")
        } catch (e: IOException) {
            assertTrue(e.message.orEmpty().contains("Refusing to contact"))
        }
        assertEquals(0, server.requestCount)
    }
}
