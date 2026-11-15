// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.SecureRandom
import java.util.Base64

/**
 * Part D's envelopes and containers against the vectors every client shares (desktop/tests/vectors/v3_story_vectors.json),
 * the story reply field on a device-key text, and the local story-key store. No engine needed: the sends are in
 * StoryDeviceKeysTest.
 */
class StoryEnvelopesTest {
    @get:Rule val folder = TemporaryFolder()

    private val vectors = Json.parseToJsonElement(File("../../desktop/tests/vectors/v3_story_vectors.json").readText()).jsonObject
    private val now = vectors.getValue("now").jsonPrimitive.long
    private val story = "dddddddd-0000-4000-8000-000000000001"

    private fun kind(d: DecodedEnvelope) = when (d) {
        is DecodedEnvelope.StoryKey -> "storykey"
        is DecodedEnvelope.StoryAnswer -> "storyanswer"
        is DecodedEnvelope.Ignore -> "ignore"
        else -> d.javaClass.simpleName
    }

    @Test fun `every story decode case is accepted or ignored as the vectors say`() {
        val cases = vectors.getValue("decode").jsonArray.map { it.jsonObject }
        assertEquals(11, cases.size)
        for (c in cases) {
            val envelope = c.getValue("envelope").jsonObject
            val name = c.getValue("name").jsonPrimitive.content
            val expect = c.getValue("accept").jsonPrimitive.content
            // The mailbox item carries the envelope's own conversation, and (a second reading) the story's id.
            val itemConversation = envelope["c"]!!.jsonPrimitive.content
            assertEquals(name, expect, kind(decodeEnvelope(envelope.toString(), itemConversation, now)))
            assertEquals(name, expect, kind(decodeEnvelope(envelope.toString(), story, now)))
        }
        val key = decodeEnvelope(cases[0].getValue("envelope").toString(), story, now) as DecodedEnvelope.StoryKey
        assertArrayEquals(ByteArray(32) { it.toByte() }, key.key.key)
        assertEquals(90_000_000L, key.key.expiresAt)
        assertEquals(story, key.key.storyId)
        val answer = decodeEnvelope(cases[6].getValue("envelope").toString(), story, now) as DecodedEnvelope.StoryAnswer
        assertEquals("poll1", answer.answer.stickerId)
        assertEquals(1L, answer.answer.rev)
        assertTrue(answer.answer.final)
        assertEquals(1, answer.answer.payload["choice"]!!.jsonPrimitive.int)
    }

    @Test fun `every RVSD case parses or is refused as the vectors say, and the good ones encode back the same`() {
        val cases = vectors.getValue("rvsd").jsonArray.map { it.jsonObject }
        assertEquals(5, cases.size)
        for (c in cases) {
            val bytes = Base64.getDecoder().decode(c.getValue("envelope").jsonPrimitive.content)
            val parsed = Rvsd.parse(bytes)
            val name = c.getValue("name").jsonPrimitive.content
            assertEquals(name, c.getValue("ok").jsonPrimitive.boolean, parsed != null)
            if (parsed == null) continue
            val expected = c.getValue("copies").jsonArray.map { it.jsonObject }
            assertEquals(expected.map { it["deviceId"]!!.jsonPrimitive.content }, parsed.map { it.deviceId })
            assertEquals(expected.map { it["sealedLength"]!!.jsonPrimitive.int }, parsed.map { it.sealed.size })
            assertArrayEquals(name, bytes, Rvsd.encode(parsed))
        }
    }

    @Test fun `RVSD refuses a device twice, a short copy, bytes left over and more than 8 KiB`() {
        val d1 = "aaaaaaaa-0000-4000-8000-000000000001"
        val good = Rvsd.encode(listOf(RvsdCopy(d1, ByteArray(48))))
        assertNotNull(Rvsd.parse(good))
        assertNull(Rvsd.parse(good + byteArrayOf(0)))
        val twice = byteArrayOf(0x52, 0x56, 0x53, 0x44, 1, 2) + good.copyOfRange(6, good.size) + good.copyOfRange(6, good.size)
        assertNull(Rvsd.parse(twice))
        val short = good.copyOf().also { it[6 + 16] = 0; it[6 + 17] = 47 }
        assertNull(Rvsd.parse(short.copyOf(short.size - 1)))
        assertTrue(runCatching { Rvsd.encode(listOf(RvsdCopy(d1, ByteArray(8200)))) }.isFailure)
        assertTrue(runCatching { Rvsd.encode(listOf(RvsdCopy(d1, ByteArray(47)))) }.isFailure)
    }

    @Test fun `story envelopes round trip, padded, and never print their key or answer`() {
        val key = ByteArray(32).also { SecureRandom().nextBytes(it) }
        val id = "bbbbbbbb-0000-4000-8000-0000000000aa"
        val text = encodeStoryKey(StoryKeyMessage(story, key, 5_000_000), id, 1000)
        assertEquals(0, text.toByteArray().size % 256)
        val back = decodeEnvelope(text, story, 2000) as DecodedEnvelope.StoryKey
        assertArrayEquals(key, back.key.key)
        assertFalse(back.toString().contains(Base64.getEncoder().encodeToString(key)))
        // Its conversation is the story: under another conversation it's ignored.
        assertTrue(decodeEnvelope(text, "11111111-1111-4111-8111-111111111111", 2000) is DecodedEnvelope.Ignore)

        val payload = storyAnswerPayload("""{"k":"question","text":"secret words"}""")
        val answer = encodeStoryAnswer(StoryAnswerMessage(story, "question1", 3, true, payload), id, 1000)
        val read = decodeEnvelope(answer, story, 2000) as DecodedEnvelope.StoryAnswer
        assertEquals(StoryAnswerMessage(story, "question1", 3, true, payload), read.answer)
        assertFalse(read.toString().contains("secret words"))
    }

    @Test fun `a text can carry a story reply, which every field must get right`() {
        val chat = "99999999-9999-4999-8999-999999999999"
        val ref = chatStoryReply("AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA", story, "image", "Photo", reaction = true)!!
        assertEquals("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", ref.authorId)
        val text = encodeText(chat, "🔥", "bbbbbbbb-0000-4000-8000-0000000000ab", 1000, MessageExtras(storyReply = ref))
        assertEquals(ref, (decodeEnvelope(text, chat, 2000) as DecodedEnvelope.Shown).storyReply)
        assertNull(chatStoryReply("nope", story, "text", "", false))
        assertNull(chatStoryReply(story, story, "song", "", false))

        fun with(reply: String) = """{"v":1,"t":"text","id":"bbbbbbbb-0000-4000-8000-0000000000ac","c":"$chat","ts":1000,"body":"hi","storyReply":$reply}"""
        val ok = """{"authorId":"$story","storyId":"$story","kind":"text","preview":"hey","reaction":false}"""
        assertTrue(decodeEnvelope(with(ok), chat, 2000) is DecodedEnvelope.Shown)
        for (bad in listOf(
            "\"x\"",
            ok.replace("\"reaction\":false", "\"reaction\":\"false\""),
            ok.replace("\"kind\":\"text\"", "\"kind\":\"song\""),
            ok.replace("\"preview\":\"hey\"", "\"preview\":\"" + "x".repeat(81) + "\""),
            ok.replace("\"storyId\":\"$story\"", "\"storyId\":\"nope\""),
            // Every field is required.
            ok.replace(",\"reaction\":false", ""),
            ok.replace("\"preview\":\"hey\",", ""),
            ok.replace("\"kind\":\"text\",", ""),
            ok.replace("\"authorId\":\"$story\",", ""),
        )) assertTrue(bad, decodeEnvelope(with(bad), chat, 2000) is DecodedEnvelope.Ignore)
        // 80 characters is the most a preview has; a reaction is a text whose body is the emoji, with reaction true.
        assertTrue(decodeEnvelope(with(ok.replace("\"preview\":\"hey\"", "\"preview\":\"" + "x".repeat(80) + "\"")), chat, 2000) is DecodedEnvelope.Shown)
        val reaction = decodeEnvelope(text, chat, 2000) as DecodedEnvelope.Shown
        assertEquals("🔥", reaction.text)
        assertTrue(reaction.storyReply!!.reaction)
    }

    // --- The store -------------------------------------------------------------------------------------------

    private val storeKey = ByteArray(32).also { SecureRandom().nextBytes(it) }
    private fun store() = StoryKeyStore(File(folder.root, "keys.bin"), storeKey)
    private val alice = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val bob = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"

    @Test fun `the store keeps keys by sender, encrypted, and past their expiry`() {
        val k = ByteArray(32) { 7 }
        val s = store()
        s.storyKey(alice, "d", StoryKeyMessage(story, k, 10_000), now = 1_000)
        assertArrayEquals(k, s.key(story, alice, 2_000))
        // Only the author's own: a key someone else sent isn't the story's.
        assertNull(s.key(story, bob, 2_000))

        val raw = File(folder.root, "keys.bin").readBytes()
        assertFalse(String(raw, Charsets.ISO_8859_1).contains(Base64.getEncoder().encodeToString(k)))
        assertFalse(String(raw, Charsets.ISO_8859_1).contains(story))
        // It opens again with the same key, and not with another.
        assertArrayEquals(k, store().key(story, alice, 2_000))
        assertNull(StoryKeyStore(File(folder.root, "keys.bin"), ByteArray(32)).key(story, alice, 2_000))

        // expiresAt alone never drops it (a highlight keeps opening), and one that arrives late is kept too.
        assertArrayEquals(k, s.key(story, alice, 1_000_000))
        val other = "dddddddd-0000-4000-8000-000000000002"
        s.storyKey(alice, "d", StoryKeyMessage(other, k, 500), now = 1_000)
        assertArrayEquals(k, s.key(other, alice, 1_000))
        s.forgetAll(listOf(story, other.uppercase()))
        assertTrue(s.storyIds().isEmpty())
        assertNull(store().key(story, alice, 2_000))
    }

    @Test fun `unlisted names stories the feed doesn't list, by sender, once their key is an hour old`() {
        assertEquals(60 * 60_000L, StoryKeyStore.LISTING_GRACE_MS)
        val s = store()
        s.storyKey(alice, "d", StoryKeyMessage(story, ByteArray(32), 1), now = 1_000)
        s.storyKey(bob, "d", StoryKeyMessage(story, ByteArray(32) { 1 }, 1), now = 1_000)
        assertTrue(s.unlisted(1_000 + 59 * 60_000L, emptySet()).isEmpty())
        assertTrue(s.unlisted(1_000 + 61 * 60_000L, setOf(story.uppercase())).isEmpty())
        assertEquals(mapOf(story to setOf(alice, bob)), s.unlisted(1_000 + 61 * 60_000L, emptySet()))
        // Asking never forgets a key by itself.
        assertNotNull(s.key(story, alice, 0))
        // Answers and debts for a story with no key here, and not in the feed, go.
        val stray = "dddddddd-0000-4000-8000-000000000009"
        s.storyAnswer(bob, StoryAnswerMessage(stray, "poll1", 1, false, storyAnswerPayload("""{"k":"poll","choice":0}""")), 1, 1)
        s.unlisted(2_000, setOf(stray))
        assertEquals(1, s.answers(stray).size)
        s.unlisted(2_000, emptySet())
        assertTrue(store().answers(stray).isEmpty())
    }

    @Test fun `a storyanswer's sticker id is 1 to 64 of letters, digits, underscore and dash, and its payload at most 4 KiB`() {
        val id = "bbbbbbbb-0000-4000-8000-0000000000ad"
        val payload = storyAnswerPayload("""{"k":"poll","choice":0}""")
        for (good in listOf("a", "Poll_1-x", "z".repeat(64))) {
            val text = encodeStoryAnswer(StoryAnswerMessage(story, good, 1, false, payload), id, 1000)
            assertEquals(good, (decodeEnvelope(text, story, 2000) as DecodedEnvelope.StoryAnswer).answer.stickerId)
        }
        fun raw(stickerId: String, rev: String = "1", body: String = payload.toString()) =
            """{"v":1,"t":"storyanswer","id":"$id","c":"$story","ts":1000,"storyId":"$story","stickerId":"$stickerId","rev":$rev,"final":false,"payload":$body}"""
        assertTrue(decodeEnvelope(raw("Poll_1-x"), story, 2000) is DecodedEnvelope.StoryAnswer)
        for (bad in listOf("", "poll 1", "a.b", "poll/1", "\u00e9t\u00e9", "z".repeat(65))) {
            assertTrue(bad, decodeEnvelope(raw(bad), story, 2000) is DecodedEnvelope.Ignore)
        }
        assertTrue(runCatching { encodeStoryAnswer(StoryAnswerMessage(story, "a b", 1, false, payload), id, 1000) }.isFailure)
        // rev is at least 1; the payload is an object of at most 4 KiB.
        assertTrue(decodeEnvelope(raw("p", rev = "0"), story, 2000) is DecodedEnvelope.Ignore)
        assertTrue(decodeEnvelope(raw("p", body = """{"t":"${"x".repeat(4090)}"}"""), story, 2000) is DecodedEnvelope.Ignore)
        assertTrue(decodeEnvelope(raw("p", body = "[1]"), story, 2000) is DecodedEnvelope.Ignore)
    }

    @Test fun `answers keep the latest revision per person, and a final one stands`() {
        val s = store()
        s.rememberOwn(story, alice, ByteArray(32), 1_000_000, 0)
        fun a(rev: Long, choice: Int, final: Boolean = false) = StoryAnswerMessage(story, "poll1", rev, final, storyAnswerPayload("""{"k":"poll","choice":$choice}"""))
        s.storyAnswer(bob, a(2, 1), 10, 10)
        s.storyAnswer(bob, a(1, 0), 11, 11)
        assertEquals(2L, s.answer(story, "poll1", bob)!!.rev)
        s.storyAnswer(bob, a(3, 0, final = true), 12, 12)
        s.storyAnswer(bob, a(4, 1), 13, 13)
        assertEquals(3L, s.answer(story, "poll1", bob)!!.rev)
        assertEquals(1, s.answers(story).size)
        // Owed viewers are kept until paid.
        s.owe(story, listOf(bob, alice), 1_000_000)
        s.paid(story, listOf(alice))
        assertEquals(listOf(bob), store().owed()[story])
        // Forgetting the story takes everything with it.
        s.forget(story)
        assertTrue(store().answers(story).isEmpty())
        assertTrue(store().owed().isEmpty())
        assertTrue(s.storyIds().isEmpty())
        assertTrue(JsonObject(emptyMap()).isEmpty())
    }
}
