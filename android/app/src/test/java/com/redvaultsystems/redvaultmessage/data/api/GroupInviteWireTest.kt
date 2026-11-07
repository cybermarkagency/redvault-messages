package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * Invite links and channels on the wire: the paths and bodies match server/src/routes/groupInviteRoutes.ts
 * and channelRoutes.ts field for field (nothing extra, and a missing group key left out rather than sent),
 * and the server's answers parse.
 */
class GroupInviteWireTest {
    private val server = MockWebServer()
    private val group = "9a8b7c6d-5e4f-4a3b-8c2d-1e0f9a8b7c6d"
    private val user = "22222222-2222-4222-8222-222222222222"
    private val linkJson = """{"id":"AbCdEf123_-x","secretCiphertext":"c2VjcmV0","secretNonce":"bm9uY2U=","approval":true,"createdAt":"2026-10-18T09:00:00.000Z"}"""

    @Before
    fun start() = server.start()

    @After
    fun stop() = server.shutdown()

    private fun api(): GroupInviteApi {
        val base = server.url("/api/").newBuilder().host("localhost").build().toString()
        return ApiClient.createGroupInvites(base, ApiClient.httpClient(SessionCookieJar(), base))
    }

    private fun next(): Pair<String, String> = server.takeRequest().let { "${it.method} ${it.path}" to it.body.readUtf8() }

    @Test
    fun membersAndAdminsManageTheLink() = runBlocking {
        val api = api()
        server.enqueue(MockResponse().setBody("""{"link":null}"""))
        server.enqueue(MockResponse().setBody("""{"link":$linkJson}"""))
        server.enqueue(MockResponse().setBody("""{"link":$linkJson}"""))
        server.enqueue(MockResponse().setBody("""{"link":$linkJson}"""))
        server.enqueue(MockResponse().setResponseCode(204))

        assertNull(api.getLink(group).link)
        val created = api.setLink(group, GroupInviteLinkInput("YXV0aA==", "c2VjcmV0", "bm9uY2U=", "cHJldmlldw==", "bm9uY2U="))
        assertTrue(created.link!!.approval)
        api.setApproval(group, GroupInviteApprovalRequest(true))
        api.setApproval(group, GroupInviteApprovalRequest(false, "a2V5", "bm9uY2U="))
        api.deleteLink(group)

        assertEquals("GET /api/group-invites/groups/$group/link" to "", next())
        assertEquals(
            "PUT /api/group-invites/groups/$group/link" to
                """{"auth":"YXV0aA==","secretCiphertext":"c2VjcmV0","secretNonce":"bm9uY2U=","previewCiphertext":"cHJldmlldw==","previewNonce":"bm9uY2U="}""",
            next(),
        )
        assertEquals("PATCH /api/group-invites/groups/$group/link" to """{"approval":true}""", next())
        assertEquals("PATCH /api/group-invites/groups/$group/link" to """{"approval":false,"wrappedKey":"a2V5","wrappedKeyNonce":"bm9uY2U="}""", next())
        assertEquals("DELETE /api/group-invites/groups/$group/link" to "", next())
    }

    @Test
    fun adminsAnswerRequestsToJoin() = runBlocking {
        val api = api()
        server.enqueue(MockResponse().setBody("""[{"userId":"$user","createdAt":"2026-10-18T09:00:00.000Z"}]"""))
        server.enqueue(MockResponse().setResponseCode(204))
        server.enqueue(MockResponse().setResponseCode(204))

        assertEquals(user, api.listRequests(group).single().userId)
        api.approve(group, user, ApproveJoinRequest("c2VhbGVk"))
        api.deny(group, user)

        assertEquals("GET /api/group-invites/groups/$group/requests" to "", next())
        assertEquals("POST /api/group-invites/groups/$group/requests/$user/approve" to """{"wrappedKey":"c2VhbGVk"}""", next())
        assertEquals("DELETE /api/group-invites/groups/$group/requests/$user" to "", next())
    }

    @Test
    fun someoneWithTheLinkOpensAndJoins() = runBlocking {
        val api = api()
        server.enqueue(
            MockResponse().setBody(
                """{"conversationId":"$group","previewCiphertext":"cHJldmlldw==","previewNonce":"bm9uY2U=","memberCount":3,"approval":false,""" +
                    """"wrappedKey":"a2V5","wrappedKeyNonce":"bm9uY2U=","member":false,"requested":false}""",
            ),
        )
        server.enqueue(MockResponse().setBody("""{"status":"joined","conversationId":"$group"}"""))
        server.enqueue(MockResponse().setBody("""{"status":"requested","conversationId":"$group"}"""))

        val preview = api.open("AbCdEf123_-x", InviteAuthRequest("YXV0aA=="))
        assertEquals(3, preview.memberCount)
        assertFalse(preview.approval)
        assertEquals("a2V5", preview.wrappedKey)
        assertEquals("joined", api.join("AbCdEf123_-x", JoinGroupInviteRequest("YXV0aA==", "c2VhbGVk")).status)
        assertEquals("requested", api.join("AbCdEf123_-x", JoinGroupInviteRequest("YXV0aA==")).status)

        assertEquals("POST /api/group-invites/links/AbCdEf123_-x/open" to """{"auth":"YXV0aA=="}""", next())
        assertEquals("POST /api/group-invites/links/AbCdEf123_-x/join" to """{"auth":"YXV0aA==","wrappedKey":"c2VhbGVk"}""", next())
        // Asking to join sends no key at all; the server reads that as null.
        assertEquals("POST /api/group-invites/links/AbCdEf123_-x/join" to """{"auth":"YXV0aA=="}""", next())
    }

    @Test
    fun aLinkThatNeedsApprovalParsesWithItsNulls() {
        val preview = ApiClient.json.decodeFromString<GroupInvitePreviewDto>(
            """{"conversationId":"$group","previewCiphertext":"cA==","previewNonce":"bg==","memberCount":1,"approval":true,""" +
                """"wrappedKey":null,"wrappedKeyNonce":null,"member":false,"requested":true}""",
        )
        assertTrue(preview.approval)
        assertNull(preview.wrappedKey)
        assertTrue(preview.requested)
    }

    @Test
    fun channelOwnersCallsMatchTheServer() = runBlocking {
        val base = server.url("/api/").newBuilder().host("localhost").build().toString()
        val api = ApiClient.createChannels(base, ApiClient.httpClient(SessionCookieJar(), base))
        val id = "11111111-2222-4333-8444-555555555555"
        val channel = """{"id":"$id","name":"News","description":"Daily","createdAt":"2026-10-18T08:00:00.000Z","lastPostAt":null}"""
        server.enqueue(MockResponse().setResponseCode(201).setBody(channel))
        server.enqueue(MockResponse().setBody(channel))
        server.enqueue(MockResponse().setResponseCode(201).setBody("""{"id":"p1","body":"Hello","createdAt":"2026-10-18T09:00:00.000Z"}"""))
        repeat(3) { server.enqueue(MockResponse().setResponseCode(204)) }

        assertEquals("Daily", api.create(ChannelDetailsRequest("News", "Daily")).description)
        api.update(id, ChannelDetailsRequest("News", "Daily"))
        assertEquals("p1", api.post(id, ChannelPostRequest("Hello")).id)
        api.deletePost(id, "p1")
        api.report(id, ChannelReportRequest("Spam"))
        api.delete(id)

        assertEquals("POST /api/channels" to """{"name":"News","description":"Daily"}""", next())
        assertEquals("PATCH /api/channels/$id" to """{"name":"News","description":"Daily"}""", next())
        assertEquals("POST /api/channels/$id/posts" to """{"body":"Hello"}""", next())
        assertEquals("DELETE /api/channels/$id/posts/p1" to "", next())
        assertEquals("POST /api/channels/$id/report" to """{"reason":"Spam"}""", next())
        assertEquals("DELETE /api/channels/$id" to "", next())
    }
}
