// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonObject
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test

/** The two file calls device-key chats add, against server/src/routes/fileRoutes.ts. */
class FileReleaseWireTest {
    private val server = MockWebServer()
    private val id = "33333333-3333-4333-8333-333333333333"

    @Before
    fun start() = server.start()

    @After
    fun stop() = server.shutdown()

    private fun api(): ApiService {
        val base = server.url("/api/").newBuilder().host("localhost").build().toString()
        return ApiClient.create(base, ApiClient.httpClient(SessionCookieJar(), base))
    }

    private fun next(): Pair<String, String> = server.takeRequest().let { "${it.method} ${it.path}" to it.body.readUtf8() }

    @Test
    fun releaseSendsAnEmptyObjectAndDeleteSendsNothing() = runBlocking {
        val api = api()
        server.enqueue(MockResponse().setBody("""{"objectId":"$id","status":"available"}"""))
        server.enqueue(MockResponse().setResponseCode(204))

        assertEquals("available", api.releaseFile(id, JsonObject(emptyMap())).status)
        api.deleteFile(id)

        assertEquals("POST /api/v1/files/$id/release" to "{}", next())
        assertEquals("DELETE /api/v1/files/$id" to "", next())
    }
}
