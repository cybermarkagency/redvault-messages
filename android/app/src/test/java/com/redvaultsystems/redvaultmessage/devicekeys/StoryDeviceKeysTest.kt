// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.goterl.lazysodium.LazySodiumJava
import com.goterl.lazysodium.SodiumJava
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.StoryAnswerCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.data.Story
import com.redvaultsystems.redvaultmessage.data.StoryDecoder
import com.redvaultsystems.redvaultmessage.data.StoryDecoding
import com.redvaultsystems.redvaultmessage.data.StoryInteractions
import com.redvaultsystems.redvaultmessage.data.StoryPublishing
import com.redvaultsystems.redvaultmessage.data.forgetUnlistedStoryKeys
import com.redvaultsystems.redvaultmessage.data.api.DELIVERY_DEVICE_KEYS
import com.redvaultsystems.redvaultmessage.data.api.StoryAnswerRequest
import com.redvaultsystems.redvaultmessage.data.api.StoryAnswerRowDto
import com.redvaultsystems.redvaultmessage.data.api.StoryAnswerSaved
import com.redvaultsystems.redvaultmessage.data.api.StoryDto
import com.redvaultsystems.redvaultmessage.data.api.StoryInteractionsApi
import com.redvaultsystems.redvaultmessage.data.api.StoryMyAnswerDto
import com.redvaultsystems.redvaultmessage.data.api.StoryTotalsRequest
import com.redvaultsystems.redvaultmessage.data.api.StoryTotalsRowDto
import com.redvaultsystems.redvaultmessage.data.model.PollSticker
import com.redvaultsystems.redvaultmessage.data.model.QuestionSticker
import com.redvaultsystems.redvaultmessage.data.model.ResultsVisibility
import com.redvaultsystems.redvaultmessage.data.model.StoryAnswer
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonObject
import org.junit.Assert.assertArrayEquals
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom
import java.time.Instant

/**
 * Part D (docs/signal-integration/12-part-d-stories.md) end to end on the JVM: the real engine, the in-memory server
 * with the story mailbox rule and the story-id rule for lists and claims, and the story code that opens what arrives.
 * Accounts: Alice posts (two devices); Bob (two devices), Carol and Dave watch; Eve is a friend who isn't a viewer;
 * Frank is a friend added after posting. Nobody has a one-to-one chat with anyone: lists, claims and certificates go
 * under the story id.
 */
class StoryDeviceKeysTest {
    @get:Rule val folder = TemporaryFolder()

    private lateinit var engine: EngineRunner
    private val server = FakeV3Server()
    private val sodium = LazySodiumJava(SodiumJava())
    private val storyCrypto = StoryCrypto(sodium)
    private val answerCrypto = StoryAnswerCrypto(sodium)

    private val alice = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val bob = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    private val carol = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
    private val dave = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"
    private val eve = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
    private val frank = "ffffffff-ffff-4fff-8fff-ffffffffffff"

    @Before fun load() {
        val call = NativeEngine.call()
        assumeTrue("the engine library isn't built for this computer", call != null)
        engine = EngineRunner(call!!)
    }

    private fun legacy() = ByteArray(32).also { SecureRandom().nextBytes(it) }

    /** One phone: its device, and the chat door with its stories. */
    private class Phone(val user: String, val name: String, val chat: V3Chat, val keys: DeviceKeysSession) {
        val stories get() = chat.stories
        suspend fun device(): V3Device = keys.chatDevice()!!
    }

    // One wrapper per phone: it stands in for that phone's Keystore, which holds the key its storage key is wrapped with.
    private val storages = HashMap<String, DeviceStorage>()
    private fun storage(name: String) = storages.getOrPut(name) { DeviceStorage(File(folder.root, name), SoftwareKeyWrapper()) }

    private fun phone(user: String, name: String, outgoing: QueueingV3Api? = null): Phone {
        val keys = DeviceKeysSession(user, ::legacy, storage(name), engine, outgoing ?: server.api(user))
        return Phone(user, name, V3Chat(keys, user, File(folder.root, "$name-v3"), outgoing), keys)
    }

    /** A new account with one device. */
    private suspend fun account(user: String, name: String, outgoing: QueueingV3Api? = null): Phone {
        setUpFirstDevice(user, legacy(), storage(name), engine, server.api(user))
        return phone(user, name, outgoing)
    }

    /** A new account with two linked devices (linked before either phone opens its session). */
    private suspend fun accountWithTwo(user: String, first: String, second: String): Pair<Phone, Phone> {
        val api = server.api(user)
        val trusted = setUpFirstDevice(user, legacy(), storage(first), engine, api)
        val joining = startNewDevice(user, storage(second), engine, api)
        approveScannedQr(trusted, qrLinkOffer(joining))
        waitForQrLink(joining, WaitOptions(intervalMs = 5, timeoutMs = 20_000))
        return phone(user, first) to phone(user, second)
    }

    /** Everyone is Alice's friend, with no one-to-one chat: the server's story-id rule is all that lets keys be claimed. */
    @Suppress("UNUSED_PARAMETER")
    private fun friendsWithAlice(vararg phones: Phone) {
        assertTrue(server.directChats.isEmpty())
    }

    private class Posted(val id: String, val key: ByteArray, val dto: StoryDto)

    /** What the author's app does before sending K: a fresh id and key, the details sealed with it, the story created with its viewers. */
    private fun post(author: Phone, viewers: List<String>, stickers: List<com.redvaultsystems.redvaultmessage.data.model.StorySticker> = emptyList(), expiresAt: Long = System.currentTimeMillis() + 86_400_000): Posted {
        val id = storyCrypto.newStoryId()
        val key = storyCrypto.newStoryKey()
        val meta = StoryPublishing.textMeta("hello from Alice", StoryBackground.Ocean, StoryFont.Typewriter, false, stickers = stickers)
        val sealed = storyCrypto.sealStoryMeta(id, key, meta)
        server.stories[id] = FakeV3Server.FakeStory(author.user, (viewers + author.user).toMutableSet(), expiresAt = expiresAt)
        val dto = StoryDto(
            id, author.user, "text", Base64Codec.encode(sealed.ciphertext), Base64Codec.encode(sealed.nonce),
            createdAt = Instant.now().toString(), expiresAt = Instant.ofEpochMilli(expiresAt).toString(), wrappedKey = null, delivery = DELIVERY_DEVICE_KEYS,
        )
        return Posted(id, key, dto)
    }

    private suspend fun open(p: Phone, posted: Posted): StoryDecoding {
        p.chat.receive()
        assertTrue(p.stories.ready())
        return StoryDecoder(storyCrypto).decode(posted.dto, ByteArray(32), ByteArray(32), deviceKeys = p.stories)
    }

    @Test fun `a story posted to three viewers, one with two devices, opens on every viewer device and the author's other device`() = runBlocking {
        val (a1, a2) = accountWithTwo(alice, "a1", "a2")
        val (b1, b2) = accountWithTwo(bob, "b1", "b2")
        val c = account(carol, "c")
        val d = account(dave, "d")
        val e = account(eve, "e")
        friendsWithAlice(a1, a2, b1, b2, c, d, e)

        assertTrue(a1.stories.ready())
        assertTrue(a1.stories.allHaveDevices(listOf(bob, carol, dave)))
        val posted = post(a1, listOf(bob, carol, dave))
        assertTrue(a1.stories.ready())
        a1.stories.rememberOwnKey(posted.id, posted.key, System.currentTimeMillis() + 86_400_000)
        val delivery = a1.stories.deliverKey(posted.id, posted.key, System.currentTimeMillis() + 86_400_000, listOf(bob, carol, dave))
        assertEquals(setOf(bob, carol, dave), delivery.delivered)
        assertTrue(delivery.owed.isEmpty())
        // Every request stays within 100 copies.
        assertTrue(server.requestSizes.all { it <= 100 })
        // Keys were claimed under the story id (there is no one-to-one chat to name).
        assertTrue(posted.id in server.claimedUnder)
        assertTrue(server.directChats.isEmpty())

        for (p in listOf(b1, b2, c, d, a2)) {
            val shown = open(p, posted)
            assertTrue("${p.name} opens it", shown is StoryDecoding.Shown)
            val story = (shown as StoryDecoding.Shown).story
            assertEquals("hello from Alice", story.meta.text)
            assertTrue(story.deviceKeys)
            assertArrayEquals(posted.key, story.key)
        }
        // The author's own device opens it with the key it kept.
        assertTrue(StoryDecoder(storyCrypto).decode(posted.dto, ByteArray(32), ByteArray(32), deviceKeys = a1.stories) is StoryDecoding.Shown)

        // Eve, a friend who isn't a viewer, gets nothing: no item, so the story stays closed (as loading at most).
        assertEquals(0, e.chat.receive()!!.stories)
        assertTrue(open(e, posted) is StoryDecoding.Waiting)
        // And the server refuses a key addressed to her.
        val refused = runCatching { a1.stories.deliverKey(posted.id, posted.key, System.currentTimeMillis() + 86_400_000, listOf(eve)) }.getOrThrow()
        assertEquals(setOf(eve), refused.owed)
        // Nothing the server holds or was sent carries K in the clear.
        val k = Base64Codec.encode(posted.key)
        assertTrue(server.sent.none { it.contains(k) })
    }

    @Test fun `a key from someone who isn't the author, or one that doesn't open the story, is never used`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val c = account(carol, "c")
        friendsWithAlice(a, b, c)
        val posted = post(a, listOf(bob, carol))
        // Carol (a viewer, not the author) can't send Bob a key for Alice's story: the server's rule refuses it...
        val forged = ByteArray(32).also { SecureRandom().nextBytes(it) }
        assertEquals(setOf(bob), c.stories.deliverKey(posted.id, forged, System.currentTimeMillis() + 60_000, listOf(bob)).owed)
        // ...and if one got through, it would be filed under Carol and never opens Alice's story.
        assertTrue(b.stories.ready())
        b.stories.openedStore()!!.storyKey(carol, "x", StoryKeyMessage(posted.id, forged, System.currentTimeMillis() + 60_000), System.currentTimeMillis())
        assertTrue(open(b, posted) is StoryDecoding.Waiting)
        // A key from the author that doesn't open the details is dropped.
        b.stories.openedStore()!!.storyKey(alice, "x", StoryKeyMessage(posted.id, forged, System.currentTimeMillis() + 60_000), System.currentTimeMillis())
        assertTrue(open(b, posted) is StoryDecoding.Skipped)
        assertNull(b.stories.key(posted.id, alice))
    }

    /** An in-memory /stories/:id/responses for the answers tests (the server's checks are its own tests). */
    private class FakeAnswers(private val server: FakeV3Server) {
        class Row(val storyId: String, val stickerId: String, val userId: String, val envelope: String, val anonymous: Boolean)

        val rows = mutableListOf<Row>()

        fun api(userId: String) = object : StoryInteractionsApi {
            override suspend fun putAnswer(storyId: String, stickerId: String, body: StoryAnswerRequest): StoryAnswerSaved {
                val bytes = Base64Codec.decode(body.envelope)
                // The server's device-keys rule: only an anonymous RVSD that parses.
                if (!Rvsd.isRvsd(bytes) || !body.anonymous || Rvsd.parse(bytes) == null) throw com.redvaultsystems.redvaultmessage.data.api.ApiException(400, "story_answer_format", "no")
                rows.removeAll { it.storyId == storyId && it.stickerId == stickerId && it.userId == userId }
                rows += Row(storyId, stickerId, userId, body.envelope, body.anonymous)
                return StoryAnswerSaved(0)
            }

            override suspend fun answers(storyId: String): List<StoryAnswerRowDto> = rows.filter { it.storyId == storyId }.map {
                StoryAnswerRowDto(it.stickerId, if (it.anonymous) null else it.userId, it.anonymous, it.envelope, 0, Instant.now().toString())
            }

            override suspend fun myAnswers(storyId: String): List<StoryMyAnswerDto> =
                rows.filter { it.storyId == storyId && it.userId == userId }.map { StoryMyAnswerDto(it.stickerId, it.anonymous, it.envelope, 0) }

            override suspend fun putTotals(storyId: String, stickerId: String, body: StoryTotalsRequest) {}
            override suspend fun totals(storyId: String): List<StoryTotalsRowDto> = emptyList()
            override suspend fun reportAnswer(storyId: String, body: JsonObject) {}
        }
    }

    private fun identity(user: String) = SelfIdentity("h", user, ByteArray(32), ByteArray(32), null, "user", "free", false)

    private fun storyOf(posted: Posted, p: Phone): Story = (StoryDecoder(storyCrypto).decode(posted.dto, ByteArray(32), ByteArray(32), deviceKeys = p.stories) as StoryDecoding.Shown).story

    private val poll = PollSticker("pollsticker1", 0.5, 0.5, 1.0, 0.0, "Pizza?", listOf("Yes", "No"), null, ResultsVisibility.Everyone)
    private val question = QuestionSticker("question1", 0.5, 0.5, 1.0, 0.0, "Ask me", true)

    @Test fun `a named answer reaches the author, latest revision wins, and the sender's other device shows it`() = runBlocking {
        val (a1, a2) = accountWithTwo(alice, "a1", "a2")
        val (b1, b2) = accountWithTwo(bob, "b1", "b2")
        friendsWithAlice(a1, a2, b1, b2)
        val posted = post(a1, listOf(bob), listOf(poll))
        assertTrue(a1.stories.ready())
        a1.stories.rememberOwnKey(posted.id, posted.key, System.currentTimeMillis() + 86_400_000)
        a1.stories.deliverKey(posted.id, posted.key, System.currentTimeMillis() + 86_400_000, listOf(bob))
        b1.chat.receive()
        val answers = FakeAnswers(server)
        val asBob = StoryInteractions(answers.api(bob), answerCrypto)
        val bobsView = storyOf(posted, b1)

        asBob.submit(identity(bob), bobsView, ByteArray(32), poll, StoryAnswer.Poll(0), b1.stories)
        asBob.submit(identity(bob), bobsView, ByteArray(32), poll, StoryAnswer.Poll(1), b1.stories)
        // Nothing named went to the server.
        assertTrue(answers.rows.isEmpty())

        // Both of Alice's devices count Bob's latest answer, under his name.
        for (author in listOf(a1, a2)) {
            author.chat.receive()
            assertTrue(author.stories.ready())
            val inbox = StoryInteractions(answers.api(alice), answerCrypto).inbox(identity(alice), storyOf(posted, author), emptyMap(), author.stories)
            assertEquals(1, inbox.entries.size)
            assertEquals(bob, inbox.entries.single().responderId)
            assertEquals(1, (inbox.entries.single().answer as StoryAnswer.Poll).choice)
            assertEquals(2, inbox.entries.single().revision)
        }
        // An older revision that turns up late changes nothing.
        a1.stories.openedStore()!!.storyAnswer(bob, StoryAnswerMessage(posted.id, poll.id, 1, false, storyAnswerPayload("""{"k":"poll","choice":0}""")), 0, 0)
        assertEquals(1, (a1.stories.answers(posted.id).single { it.userId == bob }.let { answerCrypto.parseAnswerJson(poll, it.payload.toString()) } as StoryAnswer.Poll).choice)

        // Bob's other device shows what he answered.
        b2.chat.receive()
        assertTrue(b2.stories.ready())
        val mine = StoryInteractions(answers.api(bob), answerCrypto).mine(identity(bob), storyOf(posted, b2), ByteArray(32), b2.stories)
        assertEquals(1, (mine.getValue(poll.id).answer as StoryAnswer.Poll).choice)
    }

    @Test fun `an anonymous answer is sealed by the engine to each author device, opens on each of them only, and names nobody`() = runBlocking {
        val (a1, a2) = accountWithTwo(alice, "a1", "a2")
        val b = account(bob, "b")
        val c = account(carol, "c")
        friendsWithAlice(a1, a2, b, c)
        // Both of Alice's devices speak Signal and the signed list pins each one's certificate.
        for (p in listOf(a1, a2, a1)) maintainSignal(p.device())
        maintainSignal(c.device())
        val posted = post(a1, listOf(bob), listOf(question))
        assertTrue(a1.stories.ready())
        a1.stories.rememberOwnKey(posted.id, posted.key, System.currentTimeMillis() + 86_400_000)
        a1.stories.deliverKey(posted.id, posted.key, System.currentTimeMillis() + 86_400_000, listOf(bob))
        b.chat.receive()
        val answers = FakeAnswers(server)
        StoryInteractions(answers.api(bob), answerCrypto).submit(identity(bob), storyOf(posted, b), ByteArray(32), question, StoryAnswer.Question("who are you?"), b.stories)

        // One RVSD, one copy per listed author device, nothing about Bob inside.
        val row = answers.rows.single()
        assertTrue(row.anonymous)
        val rvsd = Base64Codec.decode(row.envelope)
        val copies = Rvsd.parse(rvsd)!!
        assertEquals(2, copies.size)
        assertFalse(String(rvsd, Charsets.ISO_8859_1).contains(bob))

        assertEquals(setOf(a1.device().deviceId, a2.device().deviceId), copies.map { it.deviceId }.toSet())

        for (author in listOf(a1, a2)) {
            author.chat.receive()
            assertTrue(author.stories.ready())
            val inbox = StoryInteractions(answers.api(alice), answerCrypto).inbox(identity(alice), storyOf(posted, author), emptyMap(), author.stories)
            assertEquals(0, inbox.unreadable)
            val entry = inbox.entries.single()
            assertNull(entry.responderId)
            assertEquals("who are you?", (entry.answer as StoryAnswer.Question).text)
            // Each copy holds exactly the RVSA payload (as base64 inside the engine's seal, as on the desktop).
            val opened = author.stories.openAnonymous(rvsd)!!
            assertEquals("RVSA", String(opened, 0, 4, Charsets.US_ASCII))
            assertEquals("who are you?", (answerCrypto.openAnonymousPayload(posted.id, question, opened) as StoryAnswer.Question).text)
        }
        // A copy opens only on its own device: one device's copy relabelled for the other doesn't open.
        val a1Copy = copies.single { it.deviceId == a1.device().deviceId }
        assertNull(a2.stories.openAnonymous(Rvsd.encode(listOf(RvsdCopy(a2.device().deviceId, a1Copy.sealed)))))
        // A third device can't open it: Bob's own (no copy for it), or Carol's, even with a copy relabelled for her.
        assertTrue(b.stories.ready())
        assertNull(b.stories.openAnonymous(rvsd))
        assertTrue(c.stories.ready())
        assertNull(c.stories.openAnonymous(rvsd))
        assertNull(c.stories.openAnonymous(Rvsd.encode(listOf(RvsdCopy(c.device().deviceId, a1Copy.sealed)))))
        // Certificates were asked for under the story id; nothing private reached the server.
        assertFalse(server.sent.any { it.contains("who are you?") })
    }

    @Test fun `an author device whose certificate isn't pinned gets no copy, and with none pinned nothing is sent`() = runBlocking {
        val (a1, a2) = accountWithTwo(alice, "a1", "a2")
        val b = account(bob, "b")
        friendsWithAlice(a1, a2, b)
        // Only a1 speaks Signal: a2 has no certificate to seal against.
        maintainSignal(a1.device())
        val posted = post(a1, listOf(bob), listOf(question))
        assertTrue(a1.stories.ready())
        a1.stories.rememberOwnKey(posted.id, posted.key, System.currentTimeMillis() + 86_400_000)
        a1.stories.deliverKey(posted.id, posted.key, System.currentTimeMillis() + 86_400_000, listOf(bob))
        b.chat.receive()
        val answers = FakeAnswers(server)
        StoryInteractions(answers.api(bob), answerCrypto).submit(identity(bob), storyOf(posted, b), ByteArray(32), question, StoryAnswer.Question("hi"), b.stories)
        assertEquals(listOf(a1.device().deviceId), Rvsd.parse(Base64Codec.decode(answers.rows.single().envelope))!!.map { it.deviceId })

        // A certificate the list doesn't pin (a2 turns Signal on and uploads, but never declares it) gets no copy either.
        val a2Device = a2.device()
        a2Device.must("signalEnable")
        server.signal["$alice/${a2Device.deviceId}"] = FakeV3Server.SignalEntry(a2Device.must("signalPrekeys", "count" to 1).obj("upload"))
        assertEquals(listOf(a1.device().deviceId), Rvsd.parse(b.stories.sealAnonymous(posted.id, alice, "x".toByteArray()))!!.map { it.deviceId })
        server.signal.remove("$alice/${a1.device().deviceId}")
        val refused = runCatching { b.stories.sealAnonymous(posted.id, alice, "x".toByteArray()) }.exceptionOrNull()
        assertEquals("story_seal_unavailable", (refused as EngineException).code)
    }

    @Test fun `a reply and a reaction to a story go over device keys with the story named, never its key`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val chat = "99999999-9999-4999-8999-999999999999"
        val reply = chatStoryReply(alice, "12345678-1234-4123-8123-123456789abc", "text", "x".repeat(200), reaction = false)!!
        assertEquals(MAX_STORY_REPLY_PREVIEW_CHARS, reply.preview.length)
        assertTrue(b.chat.send(chat, false, alice, "nice!", extras = MessageExtras(storyReply = reply)) is SendOutcome.Sent)
        val reaction = reply.copy(reaction = true)
        assertTrue(b.chat.send(chat, false, alice, "🔥", extras = MessageExtras(storyReply = reaction)) is SendOutcome.Sent)

        val got = a.chat.receive()!!.received
        assertEquals(listOf("nice!", "🔥"), got.map { it.text })
        assertEquals(listOf(reply, reaction), got.map { it.storyReply })
        // On a retry it still names the story.
        val shown = a.chat.shown(chat)
        assertEquals(listOf(false, true), shown.map { it.record.storyReply?.reaction })
    }

    @Test fun `a friend added after posting gets the key, after access is given`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val f = account(frank, "f")
        friendsWithAlice(a, b, f)
        val posted = post(a, listOf(bob))
        val expiresAt = System.currentTimeMillis() + 86_400_000
        assertTrue(a.stories.ready())
        a.stories.rememberOwnKey(posted.id, posted.key, expiresAt)
        a.stories.deliverKey(posted.id, posted.key, expiresAt, listOf(bob))

        // Before access, the mailbox refuses the key and Frank is owed it.
        assertEquals(setOf(frank), a.stories.deliverKey(posted.id, posted.key, expiresAt, listOf(frank)).owed)
        assertEquals(listOf(frank), a.stories.owed()[posted.id])
        assertTrue(open(f, posted) is StoryDecoding.Waiting)

        // POST /:id/access, then the key: he opens it, and nothing is owed any more.
        server.stories.getValue(posted.id).access += frank
        assertEquals(setOf(frank), a.stories.deliverKey(posted.id, posted.key, expiresAt, a.stories.owed().getValue(posted.id)).delivered)
        assertTrue(a.stories.owed().isEmpty())
        assertTrue(open(f, posted) is StoryDecoding.Shown)
    }

    /** [keys] as if [aheadMs] had passed: what the hour's grace would let go by then. */
    private fun later(keys: V3Stories, aheadMs: Long): StoryDeviceKeys = object : StoryDeviceKeys by keys {
        override fun unlisted(listed: Set<String>) = keys.openedStore()!!.unlisted(System.currentTimeMillis() + aheadMs, listed)
    }

    @Test fun `a highlighted story keeps its key past expiry, a late friend still gets it, and it goes once nothing lists it`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        val f = account(frank, "f")
        friendsWithAlice(a, b, f)
        val soon = System.currentTimeMillis() + 3_000
        val posted = post(a, listOf(bob), expiresAt = soon)
        server.stories.getValue(posted.id).highlighted = true
        assertTrue(a.stories.ready())
        a.stories.rememberOwnKey(posted.id, posted.key, soon)
        a.stories.deliverKey(posted.id, posted.key, soon, listOf(bob))
        b.chat.receive()

        Thread.sleep(3_100)
        // Past expiresAt: still there, and the highlight still opens (the decoder reads it as any story).
        assertTrue(b.stories.ready())
        assertNotNull(b.stories.key(posted.id, alice))
        assertTrue(StoryDecoder(storyCrypto).decode(posted.dto, ByteArray(32), ByteArray(32), seen = true, deviceKeys = b.stories) is StoryDecoding.Shown)
        // A late friend on the highlight: access, then K, after expiry (the server allows it for a highlighted story).
        server.stories.getValue(posted.id).access += frank
        assertEquals(setOf(frank), a.stories.deliverKey(posted.id, posted.key, soon, listOf(frank)).delivered)
        assertTrue(open(f, posted) is StoryDecoding.Shown)

        // The feed doesn't list it, but the author's highlights do: kept, even an hour and more on.
        val highlighted: suspend (String) -> List<String> = { _ -> if (server.stories.getValue(posted.id).highlighted) listOf(posted.id.uppercase()) else emptyList() }
        assertEquals(emptySet<String>(), forgetUnlistedStoryKeys(emptySet(), later(b.stories, 2 * 3600_000L), { emptyList() }, highlighted))
        assertNotNull(b.stories.key(posted.id, alice))
        // The author's own key: its archive lists it.
        assertEquals(emptySet<String>(), forgetUnlistedStoryKeys(emptySet(), later(a.stories, 2 * 3600_000L), { listOf(posted.id) }, { emptyList() }))
        assertNotNull(a.stories.key(posted.id, alice))
        // A failed lookup forgets nothing; a 404 counts as "not listed there".
        assertNull(forgetUnlistedStoryKeys(emptySet(), later(b.stories, 2 * 3600_000L), { emptyList() }) { throw com.redvaultsystems.redvaultmessage.data.api.ApiException(0, "network_error", "offline") })
        assertNotNull(b.stories.key(posted.id, alice))
        // Out of the highlight (or access revoked): within the hour it stays, after it it goes, on disk too.
        server.stories.getValue(posted.id).highlighted = false
        assertEquals(emptySet<String>(), forgetUnlistedStoryKeys(emptySet(), b.stories, { emptyList() }, highlighted))
        assertNotNull(b.stories.key(posted.id, alice))
        assertEquals(setOf(posted.id), forgetUnlistedStoryKeys(emptySet(), later(b.stories, 2 * 3600_000L), { throw com.redvaultsystems.redvaultmessage.data.api.ApiException(404, "not_found", "no") }, highlighted))
        assertNull(b.stories.key(posted.id, alice))
        val reopened = phone(bob, "b")
        assertTrue(reopened.stories.ready())
        assertNull(reopened.stories.key(posted.id, alice))

        // Deleted by its author: gone at once.
        a.stories.forget(posted.id)
        assertNull(a.stories.key(posted.id, alice))
    }

    @Test fun `a viewer with no one-to-one chat gets the key under the story id, and only while the story allows it`() = runBlocking {
        val a = account(alice, "a")
        val b = account(bob, "b")
        friendsWithAlice(a, b)
        val posted = post(a, listOf(bob))
        val expiresAt = System.currentTimeMillis() + 86_400_000
        assertTrue(a.stories.ready())
        a.stories.rememberOwnKey(posted.id, posted.key, expiresAt)
        assertEquals(setOf(bob), a.stories.deliverKey(posted.id, posted.key, expiresAt, listOf(bob)).delivered)
        assertEquals(listOf<String?>(posted.id), server.claimedUnder.distinct())
        assertTrue(open(b, posted) is StoryDecoding.Shown)

        // Once the story is deleted, the server refuses lists and claims under its id: nothing more reaches Bob.
        val other = post(a, listOf(bob))
        server.stories.getValue(other.id).deleted = true
        assertEquals(setOf(bob), a.stories.deliverKey(other.id, other.key, expiresAt, listOf(bob)).owed)
    }

    @Test fun `story keys wait in the Tor queue, in order, and go out once it's back`() = runBlocking {
        val queue = QueueingV3Api(server.api(alice), V3SendQueue(File(folder.root, "queue"), alice, SoftwareKeyWrapper()))
        val a = account(alice, "a", outgoing = queue)
        val b = account(bob, "b")
        val c = account(carol, "c")
        friendsWithAlice(a, b, c)
        val first = post(a, listOf(bob, carol))
        val second = post(a, listOf(bob))
        val expiresAt = System.currentTimeMillis() + 86_400_000

        server.offline = true
        // Encrypted and queued: they count as delivered, nothing is owed, nothing reached the server.
        for (p in listOf(first, second)) {
            assertTrue(a.stories.ready())
            a.stories.rememberOwnKey(p.id, p.key, expiresAt)
            val d = a.stories.deliverKey(p.id, p.key, expiresAt, server.stories.getValue(p.id).access.filter { it != alice })
            assertTrue(d.owed.isEmpty())
        }
        assertTrue(a.chat.queuedMessages() >= 2)
        assertEquals(0, server.mailbox.size)
        // What waits on disk is sealed: no key in it.
        val onDisk = File(folder.root, "queue").walkTopDown().filter { it.isFile }.joinToString("") { String(it.readBytes(), Charsets.ISO_8859_1) }
        assertFalse(onDisk.contains(Base64Codec.encode(first.key)))

        server.offline = false
        assertTrue(a.chat.flushQueue())
        assertTrue(open(b, first) is StoryDecoding.Shown)
        assertTrue(open(b, second) is StoryDecoding.Shown)
        assertTrue(open(c, first) is StoryDecoding.Shown)
    }

    @Test fun `an answer is reported by its envelope, or a named device-key answer by who sent it`() {
        val stored = StoryInteractions.answerReportBody("ques0001", "RVSDenvelope", "abuse", "what they wrote", responderId = null)
        assertEquals("RVSDenvelope", (stored["envelope"] as JsonPrimitive).content)
        assertNull(stored["responderId"])
        assertEquals("what they wrote", (stored["evidence"] as JsonPrimitive).content)
        // An anonymous RVSD answer keeps its envelope even though nobody is named.
        val named = StoryInteractions.answerReportBody("ques0001", "", "abuse", " ", responderId = "0b5c6c1e-7c3e-4b7a-9a52-1f0e2d3c4b5a")
        assertEquals("0b5c6c1e-7c3e-4b7a-9a52-1f0e2d3c4b5a", (named["responderId"] as JsonPrimitive).content)
        assertNull(named["envelope"])
        assertNull(named["evidence"])
        assertTrue(runCatching { StoryInteractions.answerReportBody("ques0001", "", "abuse", null, null) }.isFailure)
    }
}
