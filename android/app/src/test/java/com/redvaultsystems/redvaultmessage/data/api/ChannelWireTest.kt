package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.coroutines.runBlocking
import okhttp3.Request
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * Channels parts two and three on the wire: the paths and bodies match
 * server/src/routes/channelRoutes.ts field for field, a private channel's
 * access token goes in X-Channel-Access (and only where the server reads it),
 * reads and view reports never carry the session cookie, and the server's
 * answers (photos, polls, checklists, media, suggestions) parse.
 */
class ChannelWireTest {
    private val server = MockWebServer()
    private val id = "11111111-2222-4333-8444-555555555555"
    private val post = "aaaaaaaa-0000-4000-8000-000000000001"
    private val token = "KQ5oUWp7EuQY/qlUwD5APFsYvK80h+szX5tNzQAxJlk="

    @Before
    fun start() = server.start()

    @After
    fun stop() = server.shutdown()

    private fun base() = server.url("/api/").newBuilder().host("localhost").build().toString()

    /** A signed-in client whose jar holds a session cookie, as after signing in. */
    private fun signedIn(): ChannelApi {
        val jar = SessionCookieJar()
        val client = ApiClient.httpClient(jar, base())
        server.enqueue(MockResponse().setResponseCode(204).addHeader("Set-Cookie", "sid=session; Path=/"))
        client.newCall(Request.Builder().url(base() + "auth/me").build()).execute().close()
        server.takeRequest()
        return ApiClient.createChannels(base(), client)
    }

    private fun take(): RecordedRequest = server.takeRequest()
    private fun RecordedRequest.line() = "$method $path"

    @Test
    fun privateChannelsAreReadWithTheirTokenAndNeverWithTheCookie() = runBlocking {
        val public = ApiClient.createPublicChannels(base(), ApiClient.publicClient(base()))
        val channel = """{"id":"$id","name":"Club","description":"","createdAt":"2026-10-18T08:00:00.000Z","lastPostAt":"2026-10-19T09:00:00.000Z",
            "isPrivate":true,"acceptSuggestions":true,"avatarUrl":"/api/public/channels/$id/avatar?v=1760860800000"}"""
        server.enqueue(MockResponse().setBody(channel))
        server.enqueue(MockResponse().setBody("""[{"id":"$post","body":"*Hi*","createdAt":"2026-10-19T09:00:00.000Z","views":12,"suggested":true,
            "poll":{"question":"Which?","options":["a","b"],"multiple":false,"quiz":true,"closed":false,"counts":[3,1],"voters":4,"correct":null,"explanation":null},
            "checklist":null,
            "media":[{"id":"m1","kind":"photo","mime":"image/jpeg","size":2048,"filename":"a.jpg","durationMs":null,"width":4000,"height":3000,
            "url":"/api/channel-media/m1?exp=1&sig=abc"}]}]"""))
        server.enqueue(MockResponse().setResponseCode(204))

        val got = public.get(id, token)
        assertTrue(got.isPrivate)
        assertTrue(got.acceptSuggestions)
        assertEquals("/api/public/channels/$id/avatar?v=1760860800000", got.avatarUrl)
        val page = public.posts(id, null, 30, token)
        val first = page.single()
        assertEquals(12, first.views)
        assertTrue(first.suggested)
        assertEquals(listOf(3, 1), first.poll!!.counts)
        assertNull(first.poll!!.correct)
        assertEquals("photo", first.media.single().kind)
        assertEquals(4000, first.media.single().width)
        public.views(ChannelViewsRequest(listOf(post)))

        val get = take()
        assertEquals("GET /api/public/channels/$id", get.line())
        assertEquals(token, get.getHeader(ACCESS_HEADER))
        val posts = take()
        assertEquals("GET /api/public/channels/$id/posts?limit=30", posts.line())
        assertEquals(token, posts.getHeader(ACCESS_HEADER))
        val views = take()
        assertEquals("POST /api/public/channels/views", views.line())
        assertEquals("""{"postIds":["$post"]}""", views.body.readUtf8())
        // A view report is post ids only: no token, no account.
        assertNull(views.getHeader(ACCESS_HEADER))
        for (r in listOf(get, posts, views)) assertNull("${r.path} carried the session cookie", r.getHeader("Cookie"))

        // Without a private link, no header at all.
        server.enqueue(MockResponse().setBody(channel))
        public.get(id)
        assertNull(take().getHeader(ACCESS_HEADER))
    }

    @Test
    fun theOwnerRunsTheChannelAsTheServerExpects() = runBlocking {
        val api = signedIn()
        val own = """{"id":"$id","name":"Club","isPrivate":true,"secretSealed":"c2VhbGVk","acceptSuggestions":false}"""
        repeat(4) { server.enqueue(MockResponse().setBody(own)) }
        server.enqueue(MockResponse().setBody("[$own]"))
        server.enqueue(MockResponse().setResponseCode(204))

        assertEquals("c2VhbGVk", api.setVisibility(id, ChannelVisibilityRequest(true, "YWNjZXNz", "c2VhbGVk")).secretSealed)
        api.setVisibility(id, ChannelVisibilityRequest(false))
        api.setSettings(id, ChannelSettingsRequest(true))
        api.setAvatar(id, ChannelAvatarRequest("/9j/"))
        assertEquals("c2VhbGVk", api.mine().single().secretSealed)
        api.removeAvatar(id)

        val bodies = (1..6).map { take().let { r -> r.line() to r.body.readUtf8() } }
        assertEquals(
            listOf(
                "PUT /api/channels/$id/visibility" to """{"isPrivate":true,"access":"YWNjZXNz","secretSealed":"c2VhbGVk"}""",
                "PUT /api/channels/$id/visibility" to """{"isPrivate":false}""",
                "PUT /api/channels/$id/settings" to """{"acceptSuggestions":true}""",
                "PUT /api/channels/$id/avatar" to """{"image":"/9j/"}""",
                "GET /api/channels/mine" to "",
                "DELETE /api/channels/$id/avatar" to "",
            ),
            bodies,
        )
    }

    @Test
    fun postsPollsChecklistsMediaAndSchedulingMatchTheServer() = runBlocking {
        val api = signedIn()
        val postJson = """{"id":"$post","body":"","createdAt":"2026-10-20T09:00:00.000Z","views":0}"""
        server.enqueue(MockResponse().setResponseCode(201).setBody("""{"id":"m1","kind":"video","size":10,"uploadUrl":"/api/channel-uploads/m1"}"""))
        repeat(4) { server.enqueue(MockResponse().setResponseCode(201).setBody(postJson)) }
        server.enqueue(MockResponse().setBody("[$postJson]"))
        server.enqueue(MockResponse().setBody(postJson))
        server.enqueue(MockResponse().setBody(postJson))

        val upload = api.startMedia(id, ChannelMediaStartRequest("video", "video/mp4", 10, "clip.mp4", 1500, 1920, 1080))
        assertEquals("/api/channel-uploads/m1", upload.uploadUrl)
        api.post(id, ChannelPostRequest("", poll = ChannelPollRequest("Which?", listOf("a", "b"), quiz = ChannelQuizRequest(1, "Because"))))
        api.post(id, ChannelPostRequest("Pick", poll = ChannelPollRequest("Which?", listOf("a", "b", "c"), multiple = true)))
        api.post(id, ChannelPostRequest("", checklist = ChannelChecklistRequest("Trip", listOf("Pack", "Go"), collaborative = true)))
        api.post(id, ChannelPostRequest("*Look*", media = listOf("m1"), publishAt = "2026-10-20T09:00:00Z"))
        api.scheduled(id)
        api.publishNow(id, post)
        api.closePoll(id, post)

        val bodies = (1..8).map { take().let { r -> r.line() to r.body.readUtf8() } }
        assertEquals(
            listOf(
                "POST /api/channels/$id/media" to
                    """{"kind":"video","mime":"video/mp4","size":10,"filename":"clip.mp4","durationMs":1500,"width":1920,"height":1080}""",
                "POST /api/channels/$id/posts" to """{"body":"","poll":{"question":"Which?","options":["a","b"],"quiz":{"correct":1,"explanation":"Because"}}}""",
                "POST /api/channels/$id/posts" to """{"body":"Pick","poll":{"question":"Which?","options":["a","b","c"],"multiple":true}}""",
                "POST /api/channels/$id/posts" to """{"body":"","checklist":{"title":"Trip","items":["Pack","Go"],"collaborative":true}}""",
                "POST /api/channels/$id/posts" to """{"body":"*Look*","media":["m1"],"publishAt":"2026-10-20T09:00:00Z"}""",
                "GET /api/channels/$id/scheduled" to "",
                "POST /api/channels/$id/posts/$post/publish" to "",
                "POST /api/channels/$id/posts/$post/close" to "",
            ),
            bodies,
        )
        // A recording's start leaves out what it doesn't know, rather than sending nulls.
        server.enqueue(MockResponse().setResponseCode(201).setBody("""{"id":"m2","uploadUrl":"/api/channel-uploads/m2"}"""))
        api.startMedia(id, ChannelMediaStartRequest("voice", "audio/mp4", 99))
        assertEquals("""{"kind":"voice","mime":"audio/mp4","size":99}""", take().body.readUtf8())
    }

    @Test
    fun votesTicksAndSuggestionsCarryThePrivateLinksToken() = runBlocking {
        val api = signedIn()
        server.enqueue(MockResponse().setBody(
            """{"poll":{"question":"Which?","options":["a","b"],"quiz":true,"closed":false,"counts":[1,0],"voters":1,"correct":0,"explanation":"Yes"},"mine":[0]}""",
        ))
        server.enqueue(MockResponse().setBody("""{"title":"Trip","items":[{"text":"Pack","done":true}],"collaborative":true}"""))
        server.enqueue(MockResponse().setResponseCode(201).setBody("""{"id":"m3","kind":"photo","size":5,"uploadUrl":"/api/channel-uploads/m3"}"""))
        server.enqueue(MockResponse().setResponseCode(201).setBody("""{"id":"s1","body":"Fan art","status":"pending","createdAt":"2026-10-19T09:00:00.000Z","media":[]}"""))
        server.enqueue(MockResponse().setBody("""[{"id":"s1","body":"Fan art","status":"approved","createdAt":"2026-10-19T09:00:00.000Z","media":[]}]"""))

        val vote = api.vote(id, post, ChannelVoteRequest(listOf(0)), token)
        assertEquals(0, vote.poll.correct)
        assertEquals(listOf(0), vote.mine)
        assertTrue(api.setChecklistItem(id, post, 0, ChannelCheckRequest(true), token).items.single().done)
        api.startSuggestionMedia(id, ChannelMediaStartRequest("photo", "image/png", 5), token)
        assertEquals("pending", api.suggest(id, ChannelSuggestionRequest("Fan art", listOf("m3")), token).status)
        assertEquals("approved", api.mySuggestions(id).single().status)

        val vr = take()
        assertEquals("POST /api/channels/$id/posts/$post/vote" to """{"options":[0]}""", vr.line() to vr.body.readUtf8())
        val tick = take()
        assertEquals("PUT /api/channels/$id/posts/$post/checklist/0" to """{"done":true}""", tick.line() to tick.body.readUtf8())
        val media = take()
        assertEquals("POST /api/channels/$id/suggestions/media", media.line())
        val suggest = take()
        assertEquals("POST /api/channels/$id/suggestions" to """{"body":"Fan art","media":["m3"]}""", suggest.line() to suggest.body.readUtf8())
        val mine = take()
        assertEquals("GET /api/channels/$id/suggestions/mine", mine.line())
        for (r in listOf(vr, tick, media, suggest)) {
            assertEquals("${r.path} lost the private link's token", token, r.getHeader(ACCESS_HEADER))
            assertEquals("sid=session", r.getHeader("Cookie"))
        }
        assertNull(mine.getHeader(ACCESS_HEADER))

        // Taking a poll vote back is an empty list; an empty suggestion's media is left out.
        server.enqueue(MockResponse().setBody("""{"poll":{"question":"Q","options":["a","b"]},"mine":[]}"""))
        server.enqueue(MockResponse().setResponseCode(201).setBody("""{"id":"s2","body":"Hi","status":"pending","createdAt":"2026-10-19T09:00:00.000Z"}"""))
        api.vote(id, post, ChannelVoteRequest(emptyList()))
        api.suggest(id, ChannelSuggestionRequest("Hi"))
        assertEquals("""{"options":[]}""", take().body.readUtf8())
        assertEquals("""{"body":"Hi"}""", take().body.readUtf8())
    }

    @Test
    fun theOwnerReviewsSuggestions() = runBlocking {
        val api = signedIn()
        server.enqueue(MockResponse().setBody("""[{"id":"s1","body":"Fan art","status":"pending","createdAt":"2026-10-19T09:00:00.000Z",
            "media":[{"id":"m3","kind":"photo","mime":"image/png","size":5,"url":"/api/channel-media/m3?exp=1&sig=x"}]}]"""))
        server.enqueue(MockResponse().setResponseCode(201).setBody("""{"id":"$post","body":"Fan art!","createdAt":"2026-10-19T10:00:00.000Z","suggested":true}"""))
        server.enqueue(MockResponse().setResponseCode(201).setBody("""{"id":"$post","body":"x","createdAt":"2026-10-21T10:00:00.000Z","suggested":true}"""))
        server.enqueue(MockResponse().setResponseCode(204))

        assertEquals("m3", api.suggestions(id).single().media.single().id)
        assertTrue(api.approveSuggestion(id, "s1", ChannelApproveRequest("Fan art!")).suggested)
        api.approveSuggestion(id, "s1", ChannelApproveRequest("x", "2026-10-21T10:00:00Z"))
        api.rejectSuggestion(id, "s2")

        assertEquals("GET /api/channels/$id/suggestions", take().line())
        take().let { assertEquals("POST /api/channels/$id/suggestions/s1/approve" to """{"body":"Fan art!"}""", it.line() to it.body.readUtf8()) }
        take().let { assertEquals("""{"body":"x","publishAt":"2026-10-21T10:00:00Z"}""", it.body.readUtf8()) }
        assertEquals("POST /api/channels/$id/suggestions/s2/reject", take().line())
    }
}
