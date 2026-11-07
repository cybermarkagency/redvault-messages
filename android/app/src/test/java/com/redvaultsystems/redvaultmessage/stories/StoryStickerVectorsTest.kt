package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.StoryAnswerCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.data.model.AnswerableSticker
import com.redvaultsystems.redvaultmessage.data.model.InboxEntry
import com.redvaultsystems.redvaultmessage.data.model.PollSticker
import com.redvaultsystems.redvaultmessage.data.model.QuestionSticker
import com.redvaultsystems.redvaultmessage.data.model.QuizSticker
import com.redvaultsystems.redvaultmessage.data.model.SliderSticker
import com.redvaultsystems.redvaultmessage.data.model.StickerTotals
import com.redvaultsystems.redvaultmessage.data.model.StoryAnswer
import com.redvaultsystems.redvaultmessage.data.model.StoryStickerParser
import com.redvaultsystems.redvaultmessage.data.model.StoryTally
import com.redvaultsystems.redvaultmessage.data.model.safeStoryLink
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * Stickers, sealed answers, sealed totals and counting, against the vectors the web app's own code made
 * (resources/vectors/story_interactive_vectors.json, from desktop/tests/vectors/make_story_interactive_vectors.ts): what the
 * web's parser keeps and drops, answers the web sealed that open here (and 26 that must stay shut), totals it sealed, and
 * its counts.
 */
class StoryStickerVectorsTest {
    private val sodium = TestSodium.sodium
    private val answers = StoryAnswerCrypto(sodium)
    private val v = vectors("story_interactive_vectors.json")
    private val storyId = v.str("storyId")
    private val storyKey = v.b64("key")
    private val alice = v.obj("alice")
    private val people = mapOf("alice" to alice, "bob" to v.obj("bob"), "carol" to v.obj("carol"))
    private val stickersRaw = v.obj("stickers")
    private val named = v.obj("stickers").obj("raw")

    /** Numbers as doubles, object keys in order: JSON that means the same compares the same. */
    private fun canon(e: JsonElement): Any? = when (e) {
        is JsonNull -> null
        is JsonObject -> e.entries.sortedBy { it.key }.associate { it.key to canon(it.value) }
        is JsonArray -> e.map(::canon)
        is JsonPrimitive -> if (e.isString) e.content else (e.doubleOrNull ?: e.content)
    }

    private fun stickerNamed(name: String): AnswerableSticker = StoryStickerParser.parse(JsonArray(listOf(named.getValue(name)))).single() as AnswerableSticker

    private fun stickerById(id: String): AnswerableSticker =
        listOf("poll", "quiz", "question", "slider").map(::stickerNamed).single { it.id == id }

    @Test
    fun stickersAreParsedTheWayTheWebParsesThem() {
        val cases = stickersRaw.arr("parseCases").map { it.jsonObject }
        assertTrue(cases.size >= 10)
        for (case in cases) {
            val parsed = StoryStickerParser.parse(case.getValue("raw")).map { StoryStickerParser.toJson(it) }
            val expected = case.arr("expected")
            assertEquals(case.str("name"), expected.map(::canon), parsed.map(::canon))
        }
    }

    @Test
    fun everyKindOfStickerIsKnown() {
        val all = StoryStickerParser.parse(stickersRaw.arr("parseCases")[0].jsonObject.getValue("raw"))
        assertEquals(9, all.size)
        assertEquals(
            listOf("PollSticker", "QuizSticker", "QuestionSticker", "SliderSticker", "CountdownSticker", "LinkSticker", "MentionSticker", "DateSticker", "LocationSticker"),
            all.map { it.javaClass.simpleName },
        )
    }

    @Test
    fun linksAreAcceptedOnlyAsTheWebWouldAcceptThemAndNeverLooser() {
        val stricter = ArrayList<String>()
        for (case in stickersRaw.arr("links").map { it.jsonObject }) {
            val input = case.str("input")
            val webResult = case.getValue("expected").let { if (it is JsonNull) null else it.jsonPrimitive.content }
            val mine = safeStoryLink(input)
            when {
                webResult == null -> assertNull("the web refuses \"$input\"", mine)
                mine == null -> stricter.add(input)
                else -> assertEquals("\"$input\"", webResult, mine)
            }
        }
        // The links this app refuses although the web would show them: spellings it won't rewrite on its own.
        assertEquals(EXPECTED_STRICTER, stricter)
    }

    @Test
    fun aStoryWithEveryStickerReadsFromItsSealedDetails() {
        val crypto = StoryCrypto(sodium)
        val story = v.obj("story")
        val expected = story.arr("expectedStickers").map(::canon)
        assertEquals(9, expected.size)
        val json = crypto.openStoryMeta(storyId, storyKey, story.b64("metaCiphertext"), story.b64("metaNonce"))
        val meta = Json.parseToJsonElement(json).jsonObject
        assertEquals(expected, StoryStickerParser.parse(meta["stickers"]).map { canon(StoryStickerParser.toJson(it)) })
        // The same story as an author's app might send it, with an unknown sticker and untidy values, sealed with another key.
        val sloppy = crypto.openStoryMeta(storyId, story.b64("sloppyKey"), story.b64("sloppyCiphertext"), story.b64("sloppyNonce"))
        assertEquals(expected, StoryStickerParser.parse(Json.parseToJsonElement(sloppy).jsonObject["stickers"]).map { canon(StoryStickerParser.toJson(it)) })
    }

    // --- Answers ---------------------------------------------------------------------------------------------

    private fun answerFrom(json: JsonObject): StoryAnswer = when (json.str("k")) {
        "poll" -> StoryAnswer.Poll(json.getValue("choice").jsonPrimitive.content.toInt())
        "quiz" -> StoryAnswer.Quiz(json.getValue("choice").jsonPrimitive.content.toInt())
        "slider" -> StoryAnswer.Slider(json.getValue("value").jsonPrimitive.content.toInt())
        else -> StoryAnswer.Question(json.str("text"))
    }

    private fun same(expected: StoryAnswer, actual: StoryAnswer) {
        assertEquals(expected.javaClass, actual.javaClass)
        when (expected) {
            is StoryAnswer.Poll -> assertEquals(expected.choice, (actual as StoryAnswer.Poll).choice)
            is StoryAnswer.Quiz -> assertEquals(expected.choice, (actual as StoryAnswer.Quiz).choice)
            is StoryAnswer.Slider -> assertEquals(expected.value, (actual as StoryAnswer.Slider).value)
            is StoryAnswer.Question -> assertEquals(expected.text, (actual as StoryAnswer.Question).text)
        }
    }

    @Test
    fun answersTheWebSealedOpenForTheAuthorAndForTheirSender() {
        val rows = v.obj("answers").arr("named").map { it.jsonObject }
        assertEquals(7, rows.size)
        for (row in rows) {
            val sticker = stickerById(row.str("stickerId"))
            val responder = people.getValue(row.str("responder"))
            val envelope = row.b64("envelope")
            val expected = answerFrom(row.obj("expected"))
            // The author opens it with the responder's public key...
            same(expected, answers.openAnswer(storyId, sticker, envelope, responder.b64("publicKey"), alice.b64("privateKey")))
            // ...and the responder opens their own again with the author's.
            same(expected, answers.openAnswer(storyId, sticker, envelope, alice.b64("publicKey"), responder.b64("privateKey")))
        }
    }

    @Test
    fun anAnswerWithoutANameOpensForTheAuthorOnly() {
        val row = v.obj("answers").arr("anonymous").single().jsonObject
        val sticker = stickerById(row.str("stickerId"))
        same(answerFrom(row.obj("expected")), answers.openAnonymousAnswer(storyId, sticker, row.b64("envelope"), alice.b64("publicKey"), alice.b64("privateKey")))
        // Nobody else can: a reader's own key opens nothing.
        assertFailsToOpen { answers.openAnonymousAnswer(storyId, sticker, row.b64("envelope"), people.getValue("bob").b64("publicKey"), people.getValue("bob").b64("privateKey")) }
    }

    private fun assertFailsToOpen(block: () -> Unit) {
        try {
            block()
        } catch (e: Exception) {
            return
        }
        fail("expected the answer to be refused")
    }

    @Test
    fun answersThatDecryptButDoNotFitTheirStickerAreRefused() {
        val bad = v.obj("answers").arr("bad").map { it.jsonObject }
        assertEquals(20, bad.size)
        for (row in bad) {
            val sticker = stickerById(row.str("stickerId"))
            val envelope = row.b64("envelope")
            val name = row.str("name")
            try {
                if (row.str("kind") == "named") {
                    answers.openAnswer(storyId, sticker, envelope, people.getValue("bob").b64("publicKey"), alice.b64("privateKey"))
                } else {
                    answers.openAnonymousAnswer(storyId, sticker, envelope, alice.b64("publicKey"), alice.b64("privateKey"))
                }
                fail("\"$name\" should be refused")
            } catch (e: com.redvaultsystems.redvaultmessage.crypto.StoryCryptoException) {
                fail("\"$name\" decrypts, so it must be refused for its content, not by the box: ${e.message}")
            } catch (e: com.redvaultsystems.redvaultmessage.data.model.AnswerException) {
                // refused for what is inside
            }
        }
    }

    @Test
    fun damagedEnvelopesAreRefused() {
        val broken = v.obj("answers").arr("broken").map { it.jsonObject }
        assertEquals(6, broken.size)
        for (row in broken) {
            val sticker = stickerById(row.str("stickerId"))
            val envelope = row.str("envelope").let { if (it.isEmpty()) ByteArray(0) else Base64Codec.decode(it) }
            assertFailsToOpen {
                if (row.str("kind") == "named") answers.openAnswer(storyId, sticker, envelope, people.getValue("bob").b64("publicKey"), alice.b64("privateKey"))
                else answers.openAnonymousAnswer(storyId, sticker, envelope, alice.b64("publicKey"), alice.b64("privateKey"))
            }
        }
    }

    @Test
    fun answersSealedHereOpenHereAndAreCheckedBeforeSealing() {
        val poll = stickerNamed("poll")
        val bob = people.getValue("bob")
        val envelope = answers.sealAnswer(storyId, poll, StoryAnswer.Poll(1), alice.b64("publicKey"), bob.b64("privateKey"))
        same(StoryAnswer.Poll(1), answers.openAnswer(storyId, poll, envelope, bob.b64("publicKey"), alice.b64("privateKey")))
        // Moved onto another story or sticker, it no longer opens.
        assertFailsToOpen { answers.openAnswer("11111111-2222-4333-8444-555555555555", poll, envelope, bob.b64("publicKey"), alice.b64("privateKey")) }
        assertFailsToOpen { answers.openAnswer(storyId, stickerNamed("quiz"), envelope, bob.b64("publicKey"), alice.b64("privateKey")) }
        // Not sealed at all if it doesn't fit the sticker.
        assertFailsToOpen { answers.sealAnswer(storyId, poll, StoryAnswer.Poll(3), alice.b64("publicKey"), bob.b64("privateKey")) }
        assertFailsToOpen { answers.sealAnswer(storyId, stickerNamed("slider"), StoryAnswer.Slider(101), alice.b64("publicKey"), bob.b64("privateKey")) }
        assertFailsToOpen { answers.sealAnswer(storyId, stickerNamed("question"), StoryAnswer.Question("   "), alice.b64("publicKey"), bob.b64("privateKey")) }
        assertFailsToOpen { answers.sealAnswer(storyId, poll, StoryAnswer.Slider(5), alice.b64("publicKey"), bob.b64("privateKey")) }
        val question = stickerNamed("question")
        val anonymous = answers.sealAnonymousAnswer(storyId, question, StoryAnswer.Question("  who? "), alice.b64("publicKey"))
        same(StoryAnswer.Question("who?"), answers.openAnonymousAnswer(storyId, question, anonymous, alice.b64("publicKey"), alice.b64("privateKey")))
    }

    // --- Totals ----------------------------------------------------------------------------------------------

    private fun totalsFrom(json: JsonObject) = StickerTotals(
        total = json.getValue("total").jsonPrimitive.content.toInt(),
        counts = (json["counts"] as? JsonArray)?.map { it.jsonPrimitive.content.toInt() },
        correct = json["correct"]?.jsonPrimitive?.content?.toInt(),
        average = json["average"]?.jsonPrimitive?.doubleOrNull,
    )

    private fun sameTotals(expected: StickerTotals, actual: StickerTotals, label: String = "") {
        assertEquals(label, expected.total, actual.total)
        assertEquals(label, expected.counts, actual.counts)
        assertEquals(label, expected.correct, actual.correct)
        assertEquals(label, expected.average, actual.average)
    }

    @Test
    fun totalsTheWebSealedOpenWithTheStoryKey() {
        val cases = v.obj("totals").arr("cases").map { it.jsonObject }
        assertEquals(5, cases.size)
        for (case in cases) {
            val sticker = stickerById(case.str("stickerId"))
            val revision = case.getValue("revision").jsonPrimitive.content.toInt()
            val opened = answers.openTotals(storyId, sticker, revision, case.b64("ciphertext"), case.b64("nonce"), storyKey)
            sameTotals(totalsFrom(case.obj("expected")), opened, case.str("name"))
            // Another revision, sticker, story or key and it no longer opens.
            assertFailsToOpen { answers.openTotals(storyId, sticker, revision + 1, case.b64("ciphertext"), case.b64("nonce"), storyKey) }
            assertFailsToOpen { answers.openTotals("11111111-2222-4333-8444-555555555555", sticker, revision, case.b64("ciphertext"), case.b64("nonce"), storyKey) }
            assertFailsToOpen { answers.openTotals(storyId, sticker, revision, case.b64("ciphertext"), case.b64("nonce"), ByteArray(32) { 3 }) }
            val other = listOf("poll", "quiz", "slider").map(::stickerNamed).first { it.id != sticker.id }
            assertFailsToOpen { answers.openTotals(storyId, other, revision, case.b64("ciphertext"), case.b64("nonce"), storyKey) }
        }
    }

    @Test
    fun totalsThatAreAuthenticButDoNotFitTheirStickerAreRefused() {
        val bad = v.obj("totals").arr("bad").map { it.jsonObject }
        assertEquals(13, bad.size)
        for (row in bad) {
            val sticker = stickerById(row.str("stickerId"))
            val revision = row.getValue("revision").jsonPrimitive.content.toInt()
            try {
                answers.openTotals(storyId, sticker, revision, row.b64("ciphertext"), row.b64("nonce"), storyKey)
                fail("\"${row.str("name")}\" should be refused")
            } catch (e: com.redvaultsystems.redvaultmessage.crypto.StoryCryptoException) {
                fail("\"${row.str("name")}\" is authentic, so it must be refused for its content, not by the AEAD: ${e.message}")
            } catch (e: com.redvaultsystems.redvaultmessage.data.model.AnswerException) {
                // refused for what is inside
            }
        }
    }

    @Test
    fun totalsAreNotSealedAtRevisionZero() {
        // Revisions start at 1: the server only takes a higher revision than the stored one, so 0 could never be published.
        assertFailsToOpen { answers.sealTotals(storyId, stickerNamed("poll").id, 0, StickerTotals(0, listOf(0, 0, 0)), storyKey) }
        assertFailsToOpen { answers.sealTotals(storyId, stickerNamed("poll").id, -1, StickerTotals(0, listOf(0, 0, 0)), storyKey) }
    }

    @Test
    fun totalsSealedHereOpenHereWithTheSameBytesAsTheWebForTheSameNonce() {
        // The additional data is part of the ciphertext: sealing the web's own totals with the web's own nonce gives its bytes.
        for (case in v.obj("totals").arr("cases").map { it.jsonObject }) {
            val sticker = stickerById(case.str("stickerId"))
            val revision = case.getValue("revision").jsonPrimitive.content.toInt()
            val sealed = answers.sealTotals(storyId, sticker.id, revision, totalsFrom(case.obj("expected")), storyKey, case.b64("nonce"))
            // Equal up to the order of the JSON's keys, which the AEAD tag covers: compare by opening instead of by bytes when they differ.
            if (!sealed.ciphertext.contentEquals(case.b64("ciphertext"))) {
                sameTotals(totalsFrom(case.obj("expected")), answers.openTotals(storyId, sticker, revision, sealed.ciphertext, sealed.nonce, storyKey), case.str("name"))
            } else {
                assertArrayEquals(case.b64("ciphertext"), sealed.ciphertext)
            }
        }
    }

    // --- Counting --------------------------------------------------------------------------------------------

    @Test
    fun answersAreCountedTheWayTheWebCountsThem() {
        val cases = v.arr("tally").map { it.jsonObject }
        assertEquals(13, cases.size)
        for (case in cases) {
            val sticker = StoryStickerParser.parse(JsonArray(listOf(case.getValue("sticker")))).single() as AnswerableSticker
            val entries = case.arr("entries").map { it.jsonObject }.map { e ->
                InboxEntry(
                    stickerById(e.str("stickerId")),
                    null,
                    answerFrom(e.obj("answer")),
                    e.getValue("at").jsonPrimitive.content.toLong(),
                    e.getValue("revision").jsonPrimitive.content.toInt(),
                    "x",
                )
            }
            sameTotals(totalsFrom(case.obj("expected")), StoryTally.tally(sticker, entries), case.str("name"))
        }
    }

    @Test
    fun theStickerKindsAreWhatTheAnswersExpect() {
        assertTrue(stickerNamed("poll") is PollSticker)
        assertTrue(stickerNamed("quiz") is QuizSticker)
        assertTrue(stickerNamed("question") is QuestionSticker)
        assertTrue(stickerNamed("slider") is SliderSticker)
        assertTrue((stickerNamed("question") as QuestionSticker).anonymous)
        assertNotNull((stickerNamed("poll") as PollSticker).closesAt)
        assertFalse((stickerNamed("quiz") as QuizSticker).explanation.isNullOrEmpty())
    }

    private companion object {
        /** Filled in from the first run: links the web shows that this app's URL parser would write differently. */
        val EXPECTED_STRICTER: List<String> = emptyList()
    }
}
