// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.privacy

import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.SessionCookieJar
import kotlinx.coroutines.runBlocking
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
 * Reading a channel tells the server nothing about who's reading: the app's public channel client sends no
 * session cookie and no identifying headers, even while signed in (the web sends these reads with
 * credentials: "omit"). Following is never sent at all; it lives in FollowedChannelsStore on the phone.
 */
class PublicChannelReadsTest {
    private val server = MockWebServer()
    private val id = "11111111-2222-4333-8444-555555555555"
    private val allowedHeaders = setOf("host", "connection", "accept-encoding")

    @Before
    fun start() = server.start()

    @After
    fun stop() = server.shutdown()

    private fun apiBase() = server.url("/api/").newBuilder().host("localhost").build().toString()

    @Test
    fun readingChannelsNeverCarriesTheSessionCookieEvenWhileSignedIn() = runBlocking {
        // Signed in: the session client's jar holds the cookie.
        val jar = SessionCookieJar()
        val signedIn = ApiClient.httpClient(jar, apiBase())
        server.enqueue(MockResponse().setResponseCode(204).addHeader("Set-Cookie", "sid=session; Path=/"))
        signedIn.newCall(Request.Builder().url(apiBase() + "auth/me").build()).execute().close()
        server.takeRequest()

        val public = ApiClient.createPublicChannels(apiBase(), ApiClient.publicClient(apiBase()))
        val channelJson = """{"id":"$id","name":"News","description":"","createdAt":"2026-10-18T08:00:00.000Z","lastPostAt":null}"""
        server.enqueue(MockResponse().setBody("[$channelJson]"))
        server.enqueue(MockResponse().setBody(channelJson))
        server.enqueue(MockResponse().setBody("""[{"id":"p1","body":"Hello","createdAt":"2026-10-18T09:00:00.000Z"}]"""))
        assertEquals("News", public.search("news", 30, 0).single().name)
        assertNull(public.get(id).lastPostAt)
        assertEquals("Hello", public.posts(id, "2026-10-18T10:00:00.000Z", 30).single().body)

        val search = server.takeRequest()
        assertEquals("/api/public/channels?q=news&limit=30&offset=0", search.path)
        val get = server.takeRequest()
        assertEquals("/api/public/channels/$id", get.path)
        val posts = server.takeRequest()
        assertEquals("/api/public/channels/$id/posts?before=2026-10-18T10%3A00%3A00.000Z&limit=30", posts.path)
        for (request in listOf(search, get, posts)) {
            assertNull("${request.path} carried the session cookie", request.getHeader("Cookie"))
            val extra = request.headers.names().map { it.lowercase() }.filterNot { it in allowedHeaders }
            assertEquals("${request.path} sent identifying headers", emptyList<String>(), extra)
        }

        // An empty search leaves q out, as the web does.
        server.enqueue(MockResponse().setBody("[]"))
        public.search(null, 30, 30)
        assertEquals("/api/public/channels?limit=30&offset=30", server.takeRequest().path)

        // Running your own channels does need the account, so those calls are signed in.
        server.enqueue(MockResponse().setBody("[]"))
        ApiClient.createChannels(apiBase(), signedIn).mine()
        val mine = server.takeRequest()
        assertEquals("/api/channels/mine", mine.path)
        assertEquals("sid=session", mine.getHeader("Cookie"))
    }

    @Test
    fun thePublicClientTalksToOurServerOnly() {
        val client = ApiClient.publicClient(apiBase())
        try {
            client.newCall(Request.Builder().url("https://tracker.example/pixel").build()).execute().close()
            fail("contacted another host")
        } catch (e: IOException) {
            assertTrue(e.message.orEmpty().contains("Refusing"))
        }
    }
}
