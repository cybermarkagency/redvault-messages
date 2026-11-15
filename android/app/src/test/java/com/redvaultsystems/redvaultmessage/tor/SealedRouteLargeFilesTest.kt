// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import okhttp3.Cookie
import okhttp3.CookieJar
import okhttp3.HttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.asRequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import okhttp3.mockwebserver.SocketPolicy
import okio.Buffer
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.nio.ByteBuffer
import java.util.concurrent.TimeUnit
import kotlin.random.Random

/**
 * Chat files bigger than one sealed message on the backup route (docs/tor-sealed-fallback.md, section 5): parts up,
 * ranges down, through a fake gateway in front of a fake chat-file API with the server's rules
 * (server/src/services/fileService.ts receiveUploadPart). The sealing is a plain test framing so the fake gateway
 * can read requests; crypto-core's own sealing is tested in SealedRouteTest and crypto-core. The same cases as the
 * desktop's (desktop/tests/sealed_route_test.cpp).
 */
class SealedRouteLargeFilesTest {
    @get:Rule val folder = TemporaryFolder()

    private val onion = MockWebServer()
    private val website = MockWebServer()
    private lateinit var route: SealedRoute
    private lateinit var client: OkHttpClient
    private val api = FakeFileApi()
    private val objectPath = "/api/v1/files/0f8fad5b-d9cb-469f-a165-70867728950e/content"

    /** A 4-byte head length, the head, then the body: never used outside tests. */
    private object PlainSealer : SealedRoute.Sealer {
        override fun seal(keyConfig: ByteArray, headJson: String, body: ByteArray) = object : SealedRoute.Sealer.Sealed {
            override val message = frame(headJson, body)
            override fun open(reply: ByteArray) = reply
            override fun discard() {}
        }
    }

    private val jar = object : CookieJar {
        override fun loadForRequest(url: HttpUrl) = listOf(Cookie.Builder().name("session").value("s").hostOnlyDomain(url.host).build())
        override fun saveFromResponse(url: HttpUrl, cookies: List<Cookie>) {}
    }

    @Before fun setUp() {
        onion.start()
        website.start()
        website.dispatcher = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest): MockResponse = api.handle(request)
        }
        val origin = onion.url("/").toString().removeSuffix("/")
        val plain = OkHttpClient.Builder().readTimeout(10, TimeUnit.SECONDS).build()
        route = SealedRoute(
            onionOrigin = origin,
            pathPrefix = "/redvaultmessages",
            gatewayUrl = website.url("/redvaultmessages/ohttp").toString(),
            keyConfig = byteArrayOf(1),
            website = plain,
            probeClient = { plain },
            probeUrl = "$origin/redvaultmessages/api/public/routing",
            cookieJar = jar,
            allowed = { true },
            onRouteChanged = {},
            probeEverySeconds = 3600,
            sealer = PlainSealer,
        )
        client = plain.newBuilder().addInterceptor(route).build()
        repeat(3) { onion.enqueue(MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AT_START)) }
        repeat(3) {
            runCatching {
                client.newCall(Request.Builder().url(api("messages")).post("{}".toRequestBody("application/json".toMediaType())).build()).execute().close()
            }
        }
        assertTrue(route.onBackup)
    }

    @After fun tearDown() {
        runCatching { onion.shutdown() }
        runCatching { website.shutdown() }
    }

    private fun api(path: String) = onion.url("/redvaultmessages/api/$path")

    @Test fun aFileOver8MbGoesUpInPartsAndComesDownInRanges() {
        val file = Random(7).nextBytes(20 * 1024 * 1024 + 12345) // three parts
        api.declared = file.size.toLong()
        val onDisk = folder.newFile().apply { writeBytes(file) }
        val url = onion.url("/redvaultmessages$objectPath")

        client.newCall(Request.Builder().url(url).put(onDisk.asRequestBody("application/octet-stream".toMediaType())).build()).execute().use {
            assertEquals(200, it.code)
            assertEquals("""{"status":"uploaded"}""", it.body!!.string())
        }
        assertArrayEquals(file, api.stored)
        assertEquals(3, api.paths.size)
        assertTrue(api.paths.all { it == objectPath })
        assertTrue(api.largestSealed <= SealedRoute.MAX_SEALED_BODY)

        api.paths.clear()
        api.largestSealed = 0
        client.newCall(Request.Builder().url(url).get().build()).execute().use {
            assertEquals(200, it.code)
            assertEquals(file.size.toString(), it.header("Content-Length"))
            assertNull(it.header("Content-Range"))
            assertArrayEquals(file, it.body!!.bytes())
        }
        assertEquals(3, api.paths.size)
        assertTrue(api.largestSealed <= SealedRoute.MAX_SEALED_BODY)
    }

    @Test fun aLostPartIsSentAgainAndARefusedPartEndsTheUpload() {
        val file = Random(8).nextBytes(9 * 1024 * 1024)
        api.declared = file.size.toLong()
        val onDisk = folder.newFile().apply { writeBytes(file) }
        val url = onion.url("/redvaultmessages$objectPath")
        api.dropNext = 1
        client.newCall(Request.Builder().url(url).put(onDisk.asRequestBody("application/octet-stream".toMediaType())).build()).execute().use {
            assertEquals(200, it.code)
        }
        assertArrayEquals(file, api.stored)

        api.refuseWith = 410
        val before = website.requestCount
        client.newCall(Request.Builder().url(url).put(onDisk.asRequestBody("application/octet-stream".toMediaType())).build()).execute().use {
            assertEquals(410, it.code)
            assertEquals("""{"error":"upload_expired"}""", it.body!!.string())
        }
        assertEquals(1, website.requestCount - before)
    }

    @Test fun otherLargeBodiesStillWaitForTheOnion() {
        val big = ByteArray(9 * 1024 * 1024)
        val result = runCatching {
            client.newCall(Request.Builder().url(api("backup/x")).put(big.toRequestBody("application/octet-stream".toMediaType())).build()).execute()
        }
        assertTrue(result.exceptionOrNull()?.message?.contains("wait for the onion") == true)
    }

    /** The fake gateway and chat-file API. */
    private class FakeFileApi {
        var declared = 0L
        var stored = ByteArray(0)
        val parts = sortedMapOf<Long, ByteArray>()
        val paths = mutableListOf<String>()
        var largestSealed = 0L
        var dropNext = 0
        var refuseWith = 0

        @Synchronized fun handle(request: RecordedRequest): MockResponse {
            val sealed = request.body.readByteArray()
            if (dropNext > 0) {
                dropNext--
                return MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AFTER_REQUEST)
            }
            largestSealed = maxOf(largestSealed, sealed.size.toLong())
            val headLength = ByteBuffer.wrap(sealed, 0, 4).int
            val head = Json.parseToJsonElement(String(sealed, 4, headLength, Charsets.UTF_8)).jsonObject
            val body = sealed.copyOfRange(4 + headLength, sealed.size)
            val headers = head["headers"]!!.jsonArray.associate { it.jsonArray[0].jsonPrimitive.content to it.jsonArray[1].jsonPrimitive.content }
            paths.add(head["path"]!!.jsonPrimitive.content)
            check(headers["cookie"] == "session=s")
            if (refuseWith != 0) return answer(refuseWith, emptyList(), """{"error":"upload_expired"}""".toByteArray())
            if (head["method"]!!.jsonPrimitive.content == "PUT") {
                val (start, end, total) = Regex("^bytes (\\d+)-(\\d+)/(\\d+)$").matchEntire(headers["content-range"]!!)!!.destructured.toList().map { it.toLong() }
                check(total == declared && start % SealedRoute.PART_BYTES == 0L && body.size.toLong() == end - start + 1)
                check(end + 1 == total || body.size.toLong() == SealedRoute.PART_BYTES)
                parts[start] = body
                if (end + 1 < total) return answer(202, emptyList(), """{"status":"pending"}""".toByteArray())
                stored = parts.values.fold(ByteArray(0)) { acc, part -> acc + part }
                return answer(200, listOf("content-type" to "application/json"), """{"status":"uploaded"}""".toByteArray())
            }
            val range = headers["range"] ?: return answer(200, emptyList(), stored)
            val (start, askedEnd) = Regex("^bytes=(\\d+)-(\\d+)$").matchEntire(range)!!.destructured.toList().map { it.toLong() }
            val end = minOf(askedEnd, stored.size - 1L)
            return answer(
                206,
                listOf(
                    "content-type" to "application/octet-stream",
                    "content-range" to "bytes $start-$end/${stored.size}",
                    "content-length" to (end - start + 1).toString(),
                ),
                stored.copyOfRange(start.toInt(), end.toInt() + 1),
            )
        }

        private fun answer(status: Int, headers: List<Pair<String, String>>, body: ByteArray): MockResponse {
            val head = buildJsonObject {
                put("status", status)
                put("headers", buildJsonArray { headers.forEach { (n, v) -> add(JsonArray(listOf(kotlinx.serialization.json.JsonPrimitive(n), kotlinx.serialization.json.JsonPrimitive(v)))) } })
            }.toString()
            return MockResponse().setResponseCode(200).setBody(Buffer().write(frame(head, body)))
        }
    }

    companion object {
        fun frame(head: String, body: ByteArray): ByteArray {
            val headBytes = head.toByteArray(Charsets.UTF_8)
            return ByteBuffer.allocate(4 + headBytes.size + body.size).putInt(headBytes.size).put(headBytes).put(body).array()
        }
    }
}
