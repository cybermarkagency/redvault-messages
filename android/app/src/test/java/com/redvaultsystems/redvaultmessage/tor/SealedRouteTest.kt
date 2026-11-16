// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import com.redvaultsystems.e2ee.NativeE2ee
import okhttp3.Cookie
import okhttp3.CookieJar
import okhttp3.HttpUrl
import okhttp3.Interceptor
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.SocketPolicy
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import java.io.IOException
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The sealed backup route's switching rules on Android (docs/tor-sealed-fallback.md), the same as the desktop's
 * (desktop/tests/sealed_route_test.cpp). Sealing needs crypto-core built for this computer (-Drv.lib): skipped without.
 */
class SealedRouteTest {
    // crypto-core/tests/vectors/sealed_test_key.json: the test key, never the live gateway's.
    private val testKey = SealedRoute.keyFromHex("0100208cfd57b6e65ffdbfbb5087ff6665b7122e1fee211913dee4ced8a3357cdfa079000400010003")

    private val onion = MockWebServer()
    private val website = MockWebServer()
    private val onionOnly = AtomicBoolean(false)
    private val torDown = AtomicBoolean(false)
    private val torConnected = AtomicBoolean(true)
    /** How many of the next requests fail as the onion transport never sending them (NotSentException). */
    private val notSent = java.util.concurrent.atomic.AtomicInteger(0)
    private val changes = mutableListOf<Boolean>()
    private lateinit var route: SealedRoute
    private lateinit var client: OkHttpClient

    private val jar = object : CookieJar {
        override fun loadForRequest(url: HttpUrl) = listOf(Cookie.Builder().name("session").value("secret-session-value").hostOnlyDomain(url.host).build())
        override fun saveFromResponse(url: HttpUrl, cookies: List<Cookie>) {}
    }

    @Before fun setUp() {
        assumeTrue("crypto-core isn't built for this computer", runCatching { NativeE2ee.load() }.isSuccess)
        onion.start()
        website.start()
        // The stand-in gateway answers 502 (it can't open requests without the gateway's private key),
        // which is enough to see what reached it.
        website.dispatcher = object : okhttp3.mockwebserver.Dispatcher() {
            override fun dispatch(request: okhttp3.mockwebserver.RecordedRequest) = MockResponse().setResponseCode(502)
        }
        val origin = onion.url("/").toString().removeSuffix("/")
        val plain = OkHttpClient.Builder().readTimeout(2, TimeUnit.SECONDS).build()
        route = SealedRoute(
            onionOrigin = origin,
            pathPrefix = "/redvaultmessages",
            gatewayUrl = website.url("/redvaultmessages/ohttp").toString(),
            keyConfig = testKey,
            website = plain,
            probeClient = { plain },
            probeUrl = "$origin/redvaultmessages/api/public/routing",
            cookieJar = jar,
            allowed = { !onionOnly.get() },
            onRouteChanged = { synchronized(changes) { changes.add(it) } },
            torConnected = { torConnected.get() },
            probeEverySeconds = 3600,
        )
        // Stands in for the Tor gate after the route: Tor down means TorUnavailableException, as in the app, and a
        // connection Tor couldn't open (TorRouting.SendWatch) means NotSentException.
        val gate = Interceptor { chain ->
            when {
                torDown.get() -> throw TorUnavailableException()
                notSent.getAndUpdate { if (it > 0) it - 1 else 0 } > 0 -> throw NotSentException(java.net.SocketException("SOCKS: Host unreachable"))
                else -> chain.proceed(chain.request())
            }
        }
        client = plain.newBuilder().addInterceptor(route).addInterceptor(gate).build()
    }

    @After fun tearDown() {
        runCatching { onion.shutdown() }
        runCatching { website.shutdown() }
    }

    private fun api(path: String) = onion.url("/redvaultmessages/api/$path")
    private fun post(path: String) = Request.Builder().url(api(path)).post("""{"text":"hello"}""".toRequestBody("application/json".toMediaType())).build()
    private fun get(path: String) = Request.Builder().url(api(path)).get().build()

    private fun send(request: Request) = client.newCall(request).execute().use { it.code }
    private fun failOnion(times: Int) {
        repeat(times) { onion.enqueue(MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AT_START)) }
        repeat(times) { runCatching { send(post("messages")) } }
    }

    @Test fun theOnionStaysInUseWhileItAnswers() {
        onion.enqueue(MockResponse().setBody("ok"))
        assertEquals(200, send(get("me")))
        assertFalse(route.onBackup)
        assertEquals(0, website.requestCount)
    }

    @Test fun threeFailuresSwitchToTheBackupRouteAndNothingReadableLeaves() {
        failOnion(3)
        assertTrue(route.onBackup)
        assertEquals(listOf(true), changes)
        // The third POST had gone out on the onion: it isn't sent again on the backup route.
        assertEquals(0, website.requestCount)
        assertTrue(runCatching { send(post("messages")) }.exceptionOrNull() is IOException) // the stand-in answers 502
        val sealed = website.takeRequest(5, TimeUnit.SECONDS)!!
        assertEquals("POST", sealed.method)
        assertEquals("/redvaultmessages/ohttp", sealed.path)
        assertEquals("message/ohttp-req", sealed.getHeader("Content-Type"))
        assertEquals(null, sealed.getHeader("Cookie"))
        val body = sealed.body.readByteArray()
        assertTrue(body.size >= 1024)
        val text = String(body, Charsets.ISO_8859_1)
        assertFalse(text.contains("secret-session-value"))
        assertFalse(text.contains("/api/messages"))
        assertFalse(text.contains("hello"))
    }

    @Test fun torBeingDownNeverSwitches() {
        torDown.set(true)
        repeat(5) { assertTrue(runCatching { send(get("me")) }.exceptionOrNull() is TorUnavailableException) }
        assertFalse(route.onBackup)
        assertEquals(0, website.requestCount)
    }

    @Test fun onlyTheOnionAddressNeverUsesTheBackupRoute() {
        onionOnly.set(true)
        failOnion(6)
        assertFalse(route.onBackup)
        assertEquals(0, website.requestCount)
    }

    @Test fun theOnionCheckMovesBackOnceItAnswers() {
        failOnion(3)
        assertTrue(route.onBackup)
        onion.enqueue(MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AT_START))
        assertFalse(route.probeOnionNow())
        assertTrue(route.onBackup)
        onion.enqueue(MockResponse().setBody("{}"))
        assertTrue(route.probeOnionNow())
        assertFalse(route.onBackup)
        assertEquals(listOf(true, false), changes)
    }

    @Test fun withoutAKeyThereIsNoBackupRoute() {
        val noKey = SealedRoute(
            onionOrigin = "http://x", pathPrefix = "", gatewayUrl = website.url("/").toString(), keyConfig = ByteArray(0),
            website = OkHttpClient(), probeClient = { OkHttpClient() }, probeUrl = "http://x/", cookieJar = jar,
            allowed = { true }, onRouteChanged = {},
        )
        val c = OkHttpClient.Builder().readTimeout(2, TimeUnit.SECONDS).addInterceptor(noKey).build()
        repeat(4) { onion.enqueue(MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AT_START)) }
        repeat(4) { runCatching { c.newCall(get("me")).execute().close() } }
        assertFalse(noKey.onBackup)
    }

    // --- A request the onion transport never sent (docs/tor-sealed-fallback.md; the desktop's rules too) ---

    @Test fun aPostThatWasNeverSentSwitchesAtOnceAndGoesByTheBackupRoute() {
        notSent.set(1)
        // The stand-in gateway answers 502: the POST reached it, sealed, so it was resent on the backup route.
        assertTrue(runCatching { send(post("auth/login")) }.exceptionOrNull() is IOException)
        assertTrue(route.onBackup)
        assertEquals(listOf(true), changes)
        assertEquals(1, website.requestCount)
        val sealed = website.takeRequest(5, TimeUnit.SECONDS)!!
        assertEquals("/redvaultmessages/ohttp", sealed.path)
        assertFalse(String(sealed.body.readByteArray(), Charsets.ISO_8859_1).contains("hello"))
        assertEquals(0, onion.requestCount)
    }

    @Test fun neverSentWhileTorIsStillConnectingDoesNotSwitchAtOnce() {
        torConnected.set(false)
        notSent.set(1)
        assertTrue(runCatching { send(post("auth/login")) }.exceptionOrNull() is NotSentException)
        assertFalse(route.onBackup)
        assertEquals(0, website.requestCount)
    }

    @Test fun neverSentWithOnlyTheOnionAddressNeverUsesTheBackupRoute() {
        onionOnly.set(true)
        notSent.set(5)
        repeat(5) { assertTrue(runCatching { send(post("auth/login")) }.exceptionOrNull() is NotSentException) }
        assertFalse(route.onBackup)
        assertEquals(0, website.requestCount)
    }

    @Test fun aStreamedBodyThatWasNeverSentIsNotResent() {
        notSent.set(1)
        val streamed = object : okhttp3.RequestBody() {
            override fun contentType() = "application/octet-stream".toMediaType()
            override fun contentLength() = -1L
            override fun writeTo(sink: okio.BufferedSink) { sink.writeUtf8("stream") }
        }
        val error = runCatching { send(Request.Builder().url(api("files")).post(streamed).build()) }.exceptionOrNull()
        assertTrue(error is NotSentException)
        assertTrue(route.onBackup) // the route still switches; only this request isn't sent again
        assertEquals(0, website.requestCount)
    }

    @Test fun startOnBackupStartsThereAndTheCheckStillMovesBack() {
        route.startOnBackup()
        assertTrue(route.onBackup)
        assertEquals(listOf(true), changes)
        onion.enqueue(MockResponse().setBody("{}"))
        assertTrue(route.probeOnionNow())
        assertFalse(route.onBackup)
        assertEquals(listOf(true, false), changes)
    }

    @Test fun startOnBackupRespectsOnlyTheOnionAddress() {
        onionOnly.set(true)
        route.startOnBackup()
        assertFalse(route.onBackup)
        assertTrue(changes.isEmpty())
    }

    @Test fun startOnBackupNeedsAKey() {
        val noKey = SealedRoute(
            onionOrigin = "http://x", pathPrefix = "", gatewayUrl = website.url("/").toString(), keyConfig = ByteArray(0),
            website = OkHttpClient(), probeClient = { OkHttpClient() }, probeUrl = "http://x/", cookieJar = jar,
            allowed = { true }, onRouteChanged = {},
        )
        noKey.startOnBackup()
        assertFalse(noKey.onBackup)
    }

    @Test fun theBackupRouteCarriesApiRequestsOnlyAndUpTo8MB() {
        failOnion(3)
        assertTrue(route.onBackup)
        assertTrue(runCatching { send(Request.Builder().url(onion.url("/redvaultmessages/desktop/latest.json")).build()) }.exceptionOrNull() is IOException)
        val big = ByteArray(9 * 1024 * 1024)
        assertTrue(runCatching { send(Request.Builder().url(api("files")).put(big.toRequestBody()).build()) }.exceptionOrNull() is IOException)
        assertEquals(0, website.requestCount)
    }
}
