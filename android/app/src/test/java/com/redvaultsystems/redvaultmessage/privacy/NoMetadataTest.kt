package com.redvaultsystems.redvaultmessage.privacy

import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.LoginRequest
import com.redvaultsystems.redvaultmessage.data.api.MarkReadRequest
import com.redvaultsystems.redvaultmessage.data.api.WebPushSubscriptionRequest
import com.redvaultsystems.redvaultmessage.data.api.SendMessageRequest
import com.redvaultsystems.redvaultmessage.data.api.SessionCookieJar
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.jsonObject
import okhttp3.Request
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import java.io.File
import java.io.IOException

/**
 * Proof that what the app actually puts on the wire says nothing about the
 * person: no device, OS, language, location or referrer headers, no extra
 * fields in request bodies, and no connection at all to any host but our own
 * server. Uses the app's real OkHttp client and Retrofit service against a
 * local server that records every request byte for byte, and writes what it
 * saw to build/privacy-proof/android-wire-capture.txt.
 */
class NoMetadataTest {
    private val server = MockWebServer()
    private val report = StringBuilder()

    // Everything an HTTP request needs to work, and nothing else.
    private val allowedHeaders = setOf("host", "connection", "accept-encoding", "content-type", "content-length", "cookie")

    @Before
    fun start() = server.start()

    @After
    fun stop() {
        server.shutdown()
        File("build/privacy-proof").mkdirs()
        File("build/privacy-proof/android-wire-capture.txt").appendText(report.toString())
    }

    private fun apiBase() = server.url("/api/").newBuilder().host("localhost").build().toString()

    private fun record(label: String, request: RecordedRequest) {
        report.append("=== $label\n${request.requestLine}\n")
        request.headers.forEach { (name, value) -> report.append("$name: $value\n") }
        val body = request.body.readUtf8()
        if (body.isNotEmpty()) report.append("\n$body\n")
        report.append('\n')
        val extra = request.headers.names().map { it.lowercase() }.filterNot { it in allowedHeaders }
        assertEquals("$label sent identifying headers", emptyList<String>(), extra)
    }

    @Test
    fun apiCallsCarryNoDeviceLocationOrUsageMetadata() = runBlocking {
        repeat(5) { server.enqueue(MockResponse().setResponseCode(204).addHeader("Set-Cookie", "sid=session; Path=/")) }
        val client = ApiClient.httpClient(SessionCookieJar(), apiBase())
        val api = ApiClient.create(apiBase(), client)

        runCatching { api.login(LoginRequest("alice", "correct horse battery staple")) }
        runCatching { api.sendMessage("conv-1", SendMessageRequest(ciphertext = "c2VhbGVk", nonce = "bm9uY2U=")) }
        runCatching { api.markRead("conv-1", MarkReadRequest("read")) }
        runCatching {
            ApiClient.createPush(apiBase(), client)
                .registerWebPush(WebPushSubscriptionRequest("https://push.example/up/abc", "BKeyFromTheDistributor", "c2VjcmV0"))
        }
        client.newCall(Request.Builder().url(apiBase() + "users/u1/avatar").build()).execute().close()

        val login = server.takeRequest()
        record("login", login)
        val send = server.takeRequest()
        record("send message", send)
        record("mark read", server.takeRequest())
        record("register push endpoint", server.takeRequest())
        record("avatar download", server.takeRequest())
        assertEquals("sid=session", send.getHeader("Cookie"))
    }

    @Test
    fun aSentMessageContainsOnlyCiphertextAndNonce() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(204))
        val api = ApiClient.create(apiBase(), ApiClient.httpClient(SessionCookieJar(), apiBase()))
        runCatching { api.sendMessage("conv-1", SendMessageRequest(ciphertext = "c2VhbGVk", nonce = "bm9uY2U=")) }

        // No timestamp, time zone, device, location or recipient list: the
        // server can't be handed what the app never sends.
        val body = ApiClient.json.parseToJsonElement(server.takeRequest().body.readUtf8()).jsonObject
        assertEquals(setOf("ciphertext", "nonce"), body.keys)
    }

    @Test
    fun noConnectionIsEverMadeToAThirdParty() {
        server.enqueue(MockResponse())
        // Same machine, different host name: "localhost" is our server, 127.0.0.1 plays a third party.
        val client = ApiClient.httpClient(SessionCookieJar(), apiBase())
        val thirdParty = server.url("/presigned-upload").newBuilder().host("127.0.0.1").build()
        check(apiBase().contains("//localhost:"))
        try {
            client.newCall(Request.Builder().url(thirdParty).build()).execute().close()
            fail("A request to a third-party host went out")
        } catch (e: IOException) {
            assertTrue(e.message!!.contains("Refusing to contact 127.0.0.1"))
        }
        assertEquals("the third party must not even see a connection", 0, server.requestCount)
        report.append("=== third-party request to $thirdParty\nrefused before connecting; requests the server received: ${server.requestCount}\n\n")
    }

    @Test
    fun redirectsToAThirdPartyAreNotFollowed() {
        server.enqueue(MockResponse().setResponseCode(302).addHeader("Location", "http://127.0.0.1:${server.port}/tracker"))
        val client = ApiClient.httpClient(SessionCookieJar(), apiBase())
        client.newCall(Request.Builder().url(apiBase() + "auth/me").build()).execute().use { assertEquals(302, it.code) }
        assertEquals(1, server.requestCount)
    }
}
