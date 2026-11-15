// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom
import java.util.UUID

/** The community routes a staff device uses (Part C2), in memory, for one community. */
class FakeCommunity(
    private val communityId: String,
    private val conversationId: String,
    private val kind: String,
    private val server: FakeV3Server,
) : CommunityDeviceApi {
    val members = mutableListOf<String>()
    val staffIds = mutableListOf<String>()
    val events = mutableListOf<MembershipEvent>()
    val waiting = mutableListOf<String>()
    val lapsed = mutableListOf<String>()
    val admitted = mutableListOf<String>()

    private fun sync() {
        server.participants[conversationId] = members.toList()
    }

    fun join(userId: String) {
        members += userId
        events += MembershipEvent(events.size + 1L, "join", userId)
        sync()
    }

    fun leave(userId: String) {
        members -= userId
        events += MembershipEvent(events.size + 1L, "leave", userId)
        sync()
    }

    override suspend fun recipients(communityId: String): List<String> = members.sorted()
    override suspend fun staff(communityId: String): List<String> = staffIds.toList()

    override suspend fun membershipTaskSummaries() =
        listOf(MembershipTaskSummary(communityId, conversationId, kind, events.lastOrNull()?.seq ?: 0, waiting.size))

    override suspend fun membershipTasks(communityId: String, after: Long, limit: Int): MembershipTasks {
        val newer = events.filter { it.seq > after }
        val page = newer.take(limit)
        return MembershipTasks(communityId, conversationId, kind, page.lastOrNull()?.seq ?: after, newer.size > limit, page, waiting.toList(), lapsed.toList())
    }

    override suspend fun admit(communityId: String, userId: String) {
        if (userId !in waiting) throw ApiException(409, "not_pending", "not waiting")
        waiting -= userId
        admitted += userId
        join(userId)
    }
}

/**
 * Part C2 on Android (docs/signal-integration/11-part-c2-communities.md), on the JVM: the real engine with the
 * `signal` feature, an in-memory server with the community send rules, and a fake of the community routes.
 * Skipped when the engine library is missing or has no Signal.
 */
class CommunityChatTest {
    @get:Rule val folder = TemporaryFolder()

    private lateinit var engine: EngineRunner
    private val server = FakeV3Server()
    private val alice = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" // owner
    private val bob = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    private val carol = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
    private val dave = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"
    private val conv = "c2c2c2c2-c2c2-4c2c-8c2c-c2c2c2c2c2c2"
    private val communityId = "f0f0f0f0-f0f0-4f0f-8f0f-f0f0f0f0f0f0"

    @Before fun load() {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        engine = EngineRunner(call!!)
        val version = engine.run(request("version"))
        assumeTrue("this engine has no Signal (build with -PredvaultE2eSignal=true)", version.json["signal"] is JsonObject)
        batchRetryDelayMs = 0
        droppedRecipients.clear()
        server.rosterIssuers[conv] = setOf(alice)
    }

    private fun legacy() = ByteArray(32).also { SecureRandom().nextBytes(it) }
    private fun storage(name: String) = DeviceStorage(File(folder.root, name), SoftwareKeyWrapper())
    private fun history(name: String, device: V3Device) = EncryptedChatHistory(File(folder.root, "$name.hist"), device.historyKey())
    private suspend fun first(user: String, name: String) = setUpFirstDevice(user, legacy(), storage(name), engine, server.api(user))
    private suspend fun second(user: String, name: String, existing: V3Device): V3Device =
        restoreWithRecoveryKey(user, createRecoveryBackup(existing), storage(name), engine, server.api(user))

    private fun ctx(kind: String, staff: Boolean) = CommunityContext(communityId, conv, kind, staff)
    private fun route(kind: String, staff: Boolean, api: CommunityDeviceApi?) = CommunityRoute(ctx(kind, staff), api)
    private val cursorFile by lazy { File(folder.root, "cursors.txt") }
    private fun cursors() = CommunityCursors(cursorFile)

    private suspend fun receive(device: V3Device, h: ChatHistory, kind: String) =
        receiveMessages(device, h, communityOf = { if (it == conv) ctx(kind, false) else null })

    private suspend fun texts(device: V3Device, h: ChatHistory, kind: String) = receive(device, h, kind).received.filter { it.op == null }.map { it.text }

    private fun bodies(user: String, device: String): List<JsonObject> =
        server.mailbox.filter { it.toUser == user && it.toDevice == device }.map { Json.parseToJsonElement(it.item["body"]!!.jsonPrimitive.content).jsonObject }

    private fun latestRoster(): List<String> = server.rosters[conv]!!.last()["members"]!!.jsonArray.map { it.jsonPrimitive.content }

    @Test fun `a group community goes over sender keys to all members, a linked device included`() = runBlocking {
        val a1 = first(alice, "a1")
        val a2 = second(alice, "a2", a1)
        val b1 = first(bob, "b1")
        val c1 = first(carol, "c1")
        for (d in listOf(a1, a2, b1, c1)) assertEquals("enabled", maintainSignal(d))
        maintainSignal(a1)
        val api = FakeCommunity(communityId, conv, "group", server).apply { staffIds += alice; join(alice); join(bob); join(carol) }
        val ha1 = history("a1", a1)
        val ha2 = history("a2", a2)
        val hb = history("b1", b1)
        val hc = history("c1", c1)

        // Before the owner's app has issued a roster, a member can't post; and a member can't issue one.
        val early = runCatching { sendGroupText(b1, hb, conv, "too early", community = route("group", false, api)) }.exceptionOrNull()
        assertTrue("$early", early is EngineException && early.code == "community_not_ready")
        val sneaky = runCatching { sendGroupText(b1, hb, conv, "as staff?", community = route("group", true, api)) }.exceptionOrNull()
        assertTrue("$sneaky", sneaky is ApiException && sneaky.code == "not_community_staff")
        assertNull(server.rosters[conv]?.takeIf { it.isNotEmpty() })

        // The owner's device runs the membership tasks: the first roster names every entitled member.
        val report = runMembershipTasks(a1, api, cursors())
        assertEquals(1, report.rosters)
        assertEquals(listOf(alice, bob, carol), latestRoster())
        // Nothing new: the next round does nothing.
        assertEquals(MembershipTaskReport(), runMembershipTasks(a1, api, cursors()))

        // bob (whose refused roster was undone on his phone) posts to everyone.
        assertTrue(sendGroupText(b1, hb, conv, "hello from bob", community = route("group", false, api)) is SendOutcome.Sent)
        for ((user, device) in listOf(alice to a1.deviceId, alice to a2.deviceId, carol to c1.deviceId)) {
            assertEquals("rv-signal-group-v1", bodies(user, device).last()["protocol"]!!.jsonPrimitive.content)
        }
        assertEquals(listOf("hello from bob"), texts(a1, ha1, "group"))
        assertEquals(listOf("hello from bob"), texts(a2, ha2, "group"))
        assertEquals(listOf("hello from bob"), texts(c1, hc, "group"))

        assertTrue(sendGroupText(a1, ha1, conv, "welcome", community = route("group", true, api)) is SendOutcome.Sent)
        val copy = receive(a2, ha2, "group").received.single()
        assertEquals("out" to "welcome", copy.direction to copy.text)
        assertEquals(listOf("welcome"), texts(b1, hb, "group"))
        assertEquals(listOf("welcome"), texts(c1, hc, "group"))
        assertTrue(server.sent.none { it.contains("hello from bob") || it.contains("welcome") })
    }

    @Test fun `a channel reaches its members, members cannot post, and a removed member reads nothing after the rotation`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val c1 = first(carol, "c1")
        for (d in listOf(a1, b1, c1)) assertEquals("enabled", maintainSignal(d))
        val api = FakeCommunity(communityId, conv, "channel", server).apply { staffIds += alice; join(alice); join(bob); join(carol) }
        val ha = history("a1", a1)
        val hb = history("b1", b1)
        val hc = history("c1", c1)

        assertEquals(1, runMembershipTasks(a1, api, cursors()).rosters)
        assertEquals(listOf(alice), latestRoster()) // senders only
        assertTrue(sendGroupText(a1, ha, conv, "first post", community = route("channel", true, api)) is SendOutcome.Sent)
        assertEquals(listOf("first post"), texts(b1, hb, "channel"))
        assertEquals(listOf("first post"), texts(c1, hc, "channel"))

        // A member's post is refused before anything leaves the phone.
        val before = server.requestSizes.size
        val post = runCatching { sendGroupText(b1, hb, conv, "member post", community = route("channel", false, api)) }.exceptionOrNull()
        assertTrue("$post", post is EngineException && post.code == "cannot_post")
        assertEquals(before, server.requestSizes.size)

        // A member's reaction goes to the owner (pairwise) and never to other members.
        val postId = hb.messages(conv).single { it.op == null }.id
        sendChatOp(b1, hb, conv, true, "", ChatOp.reaction(postId, "👍"), community = route("channel", false, api))
        assertEquals(0, server.pending(carol, c1.deviceId))
        assertEquals(ChatOp.REACTION, receive(a1, ha, "channel").received.single().op?.t)

        // carol leaves: the owner's device rotates (next epoch, same staff) and shares the new key with who remains.
        api.leave(carol)
        server.notEntitled[conv] = mutableSetOf(carol)
        val report = runMembershipTasks(a1, api, cursors())
        assertEquals(1, report.rotations)
        assertEquals(2, server.rosters[conv]!!.size)
        assertEquals(listOf(alice), latestRoster())
        assertEquals(1, report.shared)

        sendGroupText(a1, ha, conv, "after carol left", community = route("channel", true, api))
        assertEquals(0, server.pending(carol, c1.deviceId))
        // Even handed the ciphertext, carol's phone can't open it: the new sender key never reached her.
        val leaked = server.mailbox.last { it.toUser == bob }
        val copyId = UUID.randomUUID().toString()
        server.mailbox += FakeV3Server.Mail(copyId, carol, c1.deviceId, JsonObject(leaked.item + ("id" to JsonPrimitive(copyId))))
        assertEquals(listOf("after carol left"), texts(b1, hb, "channel"))
        val atCarol = receive(c1, hc, "channel")
        assertTrue(atCarol.received.isEmpty())
        assertTrue(hc.messages(conv).none { it.text == "after carol left" })
    }

    @Test fun `an account that is not entitled is dropped and the batch goes again, then the owner's tasks fix the roster`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val c1 = first(carol, "c1")
        for (d in listOf(a1, b1, c1)) maintainSignal(d)
        val api = FakeCommunity(communityId, conv, "group", server).apply { staffIds += alice; join(alice); join(bob); join(carol) }
        runMembershipTasks(a1, api, cursors())
        val ha = history("a1", a1)
        val hb = history("b1", b1)

        // carol's payment lapsed; the roster still names her, and the server refuses anything addressed to her.
        server.notEntitled[conv] = mutableSetOf(carol)
        assertTrue(sendGroupText(b1, hb, conv, "while carol lapsed", community = route("group", false, api)) is SendOutcome.Sent)
        assertEquals(listOf("while carol lapsed"), texts(a1, ha, "group"))
        assertEquals(0, server.pending(carol, c1.deviceId))
        assertEquals(setOf(carol), droppedRecipients[conv])

        // The owner's next round leaves her out of the roster even with no new event.
        api.members -= carol
        api.lapsed += carol
        assertEquals(1, runMembershipTasks(a1, api, cursors()).rosters)
        assertEquals(listOf(alice, bob), latestRoster())
        assertNull(droppedRecipients[conv])
    }

    @Test fun `waiting members are admitted and get what comes after, new devices too, nothing from before`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        val d1 = first(dave, "d1")
        for (d in listOf(a1, b1, d1)) maintainSignal(d)
        val api = FakeCommunity(communityId, conv, "group", server).apply { staffIds += alice; join(alice); join(bob) }
        runMembershipTasks(a1, api, cursors())
        val ha = history("a1", a1)
        val hb = history("b1", b1)
        val hd = history("d1", d1)
        sendGroupText(a1, ha, conv, "before dave", community = route("group", true, api))
        assertEquals(listOf("before dave"), texts(b1, hb, "group"))

        api.waiting += dave
        val report = runMembershipTasks(a1, api, cursors())
        assertEquals(1, report.admitted)
        assertEquals(listOf(dave), api.admitted)
        assertEquals(listOf(alice, bob, dave), latestRoster())

        // bob links a second phone.
        val b2 = second(bob, "b2", b1)
        maintainSignal(b2)
        maintainSignal(b1)
        val hb2 = history("b2", b2)

        sendGroupText(a1, ha, conv, "after dave", community = route("group", true, api))
        assertEquals(listOf("after dave"), texts(d1, hd, "group"))
        assertEquals(listOf("after dave"), texts(b2, hb2, "group"))
        // What came before isn't there; the chat says so at the top.
        assertTrue(hd.messages(conv).none { it.text == "before dave" })
        assertTrue(hb2.messages(conv).none { it.text == "before dave" })
        assertEquals("Messages from before you joined aren't available", beforeYouJoinedNotice(true))
        assertNull(beforeYouJoinedNotice(false))
    }

    @Test fun `a post that arrives pairwise in a community isn't shown`() = runBlocking {
        val a1 = first(alice, "a1")
        val b1 = first(bob, "b1")
        for (d in listOf(a1, b1)) maintainSignal(d)
        server.participants[conv] = listOf(alice, bob)
        // A plain one-to-one send naming the community's conversation: only changes and receipts may come this way.
        sendText(a1, history("a1", a1), conv, bob, "sideways")
        val report = receive(b1, history("b1", b1), "channel")
        assertTrue(report.received.isEmpty())
        assertTrue("community_pairwise_post" in report.dropped)
    }

    // --- Batching (no engine): the request sizes and retries, against a fake post ---

    @Test fun `sends go out at most 100 devices per request, in order, and a failed batch is tried again`() = runBlocking {
        batchRetryDelayMs = 0
        val devices = (1..250).map { "user-$it" to "device-$it" }
        val requests = mutableListOf<List<Pair<String, String>>>()
        var failOnce = true
        postInBatches(devices, { it.first }) { batch ->
            if (requests.size == 1 && failOnce) {
                failOnce = false
                throw ApiException(0, "network_error", "offline")
            }
            requests += batch
        }
        assertEquals(listOf(100, 100, 50), requests.map { it.size })
        assertEquals(devices, requests.flatten())
    }

    @Test fun `a batch refused for an account that isn't entitled goes once more without it`() = runBlocking {
        val devices = (1..120).map { "user-${it % 7}" to "device-$it" }
        val requests = mutableListOf<List<Pair<String, String>>>()
        val dropped = postInBatches(devices, { it.first }) { batch ->
            if (batch.any { it.first == "user-3" }) {
                throw ApiException(409, "recipient_not_entitled", "no", buildJsonObject { put("userIds", kotlinx.serialization.json.JsonArray(listOf(JsonPrimitive("user-3")))) })
            }
            requests += batch
        }
        assertEquals(setOf("user-3"), dropped)
        assertTrue(requests.flatten().none { it.first == "user-3" })
        assertEquals(devices.filter { it.first != "user-3" }, requests.flatten())
        assertTrue(requests.all { it.size <= 100 })
        // A second refusal of the same request is a real failure.
        val again = runCatching {
            postInBatches(listOf("a" to "1", "b" to "2"), { it.first }) { _ ->
                throw ApiException(409, "recipient_not_entitled", "no", buildJsonObject { put("userIds", kotlinx.serialization.json.JsonArray(listOf(JsonPrimitive("a")))) })
            }
        }.exceptionOrNull()
        assertTrue("$again", again is ApiException)
    }

    @Test fun `a channel post to more than 100 member devices goes in batches`() = runBlocking {
        // One real owner device and 101 real reader devices: the sender-key messages and the post itself each go out
        // as a request of 100 devices and one of 1, in order; everyone reads the post.
        val a1 = first(alice, "a1")
        maintainSignal(a1)
        val api = FakeCommunity(communityId, conv, "channel", server).apply { staffIds += alice; join(alice) }
        val readers = (1..101).map { i ->
            val user = "eeeeeeee-eeee-4eee-8eee-${"%012d".format(i)}"
            first(user, "r$i").also { maintainSignal(it); api.join(user) }
        }
        server.requestSizes.clear()
        // The owner's tasks share the sender key with the 101 newcomers: two requests.
        assertEquals(101, runMembershipTasks(a1, api, cursors()).shared)
        assertEquals(listOf(100, 1), server.requestSizes)
        server.requestSizes.clear()
        // The post: encrypted once, sent in two requests (everyone already has the key).
        sendGroupText(a1, history("a1", a1), conv, "to the readers", community = route("channel", true, api))
        assertEquals(listOf(100, 1), server.requestSizes)
        for (p in listOf(readers.first(), readers[57], readers.last())) {
            assertEquals(listOf("to the readers"), texts(p, history("h-${p.userId}", p), "channel"))
        }
    }
}
