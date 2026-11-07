package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileDecryptException
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.data.SkipReason
import com.redvaultsystems.redvaultmessage.data.Story
import com.redvaultsystems.redvaultmessage.data.StoryDecoder
import com.redvaultsystems.redvaultmessage.data.StoryDecoding
import com.redvaultsystems.redvaultmessage.data.StoryFriend
import com.redvaultsystems.redvaultmessage.data.api.StoryAuthorGroup
import com.redvaultsystems.redvaultmessage.data.api.StoryFeed
import com.redvaultsystems.redvaultmessage.data.api.StoryFeedParser
import com.redvaultsystems.redvaultmessage.data.api.StoryDto
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefsParser
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.security.MessageDigest
import java.time.Instant

/**
 * A feed as the server would hand it to a viewer, every story sealed by the web app's own code
 * (resources/vectors/story_wire_vectors.json, made by make_android_story_vectors.ts), and what the web's own
 * decodeFeed made of it. This app must show what the web shows, except the kinds it doesn't know, and refuse
 * what the web refuses; a story it skips must never hide the author's others.
 */
class StoryFeedVectorsTest {
    private val sodium = TestSodium.sodium
    private val decoder = StoryDecoder(StoryCrypto(sodium))
    private val v = vectors("story_wire_vectors.json")
    private val ids = v.obj("ids")
    private val keys = v.obj("keys")
    private val bobPublic = keys.obj("bob").b64("publicKey")
    private val bobPrivate = keys.obj("bob").b64("privateKey")
    private val webVisible = v.arr("webVisible").map { it.jsonObject }
    private val audioId = v.arr("skippedByAndroid").map { it.jsonObject }.single().str("id")

    private fun friends(): Map<String, StoryFriend> =
        v.arr("friendsOfBob").map { it.jsonObject }.associate { f ->
            f.str("userId") to StoryFriend(f.str("userId"), f.str("username"), null, f.b64("publicKey"))
        }

    private fun feed() = StoryFeedParser.parse(v.obj("feedForBob"))

    private fun decoded(seenHere: Set<String> = emptySet()) = decoder.decodeFeed(feed(), bobPublic, bobPrivate, friends(), seenHere)

    private fun dtoOf(id: String): StoryDto = feed().authors.flatMap { it.stories }.single { it.id == id }

    private val shown get() = decoded().authors.flatMap { it.stories }

    @Test
    fun showsWhatTheWebShowsExceptTheKindsItDoesNotKnow() {
        val expected = webVisible.map { it.str("id") }.filter { it != audioId }
        assertEquals(expected, shown.map { it.id })
        assertEquals(listOf("alice", "carol"), decoded().authors.map { it.author.handle })
    }

    @Test
    fun readsEachStoryTheWayTheWebDoes() {
        for (web in webVisible.filter { it.str("id") != audioId }) {
            val story = shown.single { it.id == web.str("id") }
            val meta = web.obj("meta")
            assertEquals(web.str("authorId"), story.authorId)
            assertEquals(web.str("kind"), story.kind.wire)
            assertEquals(web.getValue("allowReplies").jsonPrimitive.booleanOrNull, story.allowReplies)
            assertEquals(web.getValue("seen").jsonPrimitive.booleanOrNull, story.seen)
            assertEquals(web.getValue("createdAt").jsonPrimitive.doubleOrNull!!.toLong(), story.createdAtMs)
            assertEquals(web.getValue("expiresAt").jsonPrimitive.doubleOrNull!!.toLong(), story.expiresAtMs)
            assertArrayEquals(web.b64("key"), story.key)
            assertEquals(meta.getValue("closeFriends").jsonPrimitive.booleanOrNull, story.meta.closeFriends)
            assertEquals(meta["text"]?.str(), story.meta.text)
            assertEquals(meta["background"]?.str(), story.meta.background?.wire)
            assertEquals(meta["font"]?.str(), story.meta.font?.wire)
            assertOverlays(meta.arr("overlays"), story)
            assertCaptions(meta["captions"]?.jsonArray ?: JsonArray(emptyList()), story)
            assertMedia(meta["media"]?.jsonObject, story)
        }
    }

    private fun assertOverlays(web: JsonArray, story: Story) {
        assertEquals(web.size, story.meta.overlays.size)
        web.map { it.jsonObject }.zip(story.meta.overlays).forEachIndexed { i, (w, o) ->
            val at = "story ${story.id} overlay $i"
            assertEquals(at, w.str("type"), if (o.isEmoji) "emoji" else "text")
            assertEquals(at, w.str("text"), o.text)
            assertEquals(at, w.getValue("x").jsonPrimitive.doubleOrNull!!, o.x, 0.0)
            assertEquals(at, w.getValue("y").jsonPrimitive.doubleOrNull!!, o.y, 0.0)
            assertEquals(at, w.getValue("scale").jsonPrimitive.doubleOrNull!!, o.scale, 0.0)
            assertEquals(at, w.getValue("rotation").jsonPrimitive.doubleOrNull!!, o.rotation, 0.0)
            assertEquals(at, w.str("color"), o.color.wire)
            assertEquals(at, w.getValue("boxed").jsonPrimitive.booleanOrNull, o.boxed)
            assertEquals(at, w.str("font"), o.font.wire)
            assertEquals(at, w["anim"]?.str(), o.anim?.wire)
        }
    }

    private fun assertCaptions(web: JsonArray, story: Story) {
        assertEquals(web.size, story.meta.captions.size)
        web.map { it.jsonObject }.zip(story.meta.captions).forEach { (w, c) ->
            assertEquals(w.getValue("startMs").jsonPrimitive.doubleOrNull!!.toLong(), c.startMs)
            assertEquals(w.getValue("endMs").jsonPrimitive.doubleOrNull!!.toLong(), c.endMs)
            assertEquals(w.str("text"), c.text)
        }
    }

    private fun assertMedia(web: JsonObject?, story: Story) {
        val m = story.meta.media
        if (web == null) return assertNull(m)
        assertNotNull(m)
        assertEquals(web.str("key"), m!!.key)
        assertEquals(web.str("header"), m.header)
        assertEquals(web.str("sha256"), m.sha256)
        assertEquals(web.getValue("encryptedSize").jsonPrimitive.doubleOrNull!!.toLong(), m.encryptedSize)
        assertEquals(web.getValue("chunkSize").jsonPrimitive.doubleOrNull!!.toLong(), m.chunkSize)
        assertEquals(web.str("mime"), m.mime)
        assertEquals(web.getValue("width").jsonPrimitive.doubleOrNull!!.toInt(), m.width)
        assertEquals(web.getValue("height").jsonPrimitive.doubleOrNull!!.toInt(), m.height)
        assertEquals(web["durationMs"]?.jsonPrimitive?.doubleOrNull?.toLong(), m.durationMs)
    }

    @Test
    fun anAudioStoryAndAnUnknownKindAreSkippedWithoutHidingTheAuthorsOtherStories() {
        for (id in listOf(audioId, "5a000000-0000-4000-8000-000000000005")) {
            val outcome = decoder.decode(dtoOf(id), friends().getValue(ids.str("alice")).publicKey, bobPrivate)
            assertEquals(SkipReason.UnsupportedKind, (outcome as StoryDecoding.Skipped).reason)
        }
        // The author's text, photo and video stories are all still there, in the order they were posted.
        val alice = decoded().authors.first { it.author.handle == "alice" }.stories
        assertEquals(listOf(StoryKind.Text, StoryKind.Image, StoryKind.Video, StoryKind.Text), alice.map { it.kind })
    }

    @Test
    fun refusesWhatTheWebRefuses() {
        val shownIds = shown.map { it.id }.toSet()
        for (id in v.arr("refusedEverywhere").map { it.str() }) assertFalse("story $id must stay hidden", id in shownIds)
        val alicePublic = friends().getValue(ids.str("alice")).publicKey
        val reasons = mapOf(
            "5a000000-0000-4000-8000-000000000007" to SkipReason.Malformed, // an overlay type nobody knows
            "5a000000-0000-4000-8000-000000000008" to SkipReason.CannotOpen, // the key of another story
            "5a000000-0000-4000-8000-000000000009" to SkipReason.CannotOpen, // a changed byte
            "5a000000-0000-4000-8000-000000000010" to SkipReason.Malformed, // the server says text, the details say image
            "5a000000-0000-4000-8000-000000000011" to SkipReason.CannotOpen, // a key sealed by someone else
            "5a000000-0000-4000-8000-000000000013" to SkipReason.Malformed, // details that aren't an object
            "5a000000-0000-4000-8000-000000000014" to SkipReason.Malformed, // a text story with only blanks
            "5a000000-0000-4000-8000-000000000015" to SkipReason.Malformed, // a photo that is an svg
        )
        for ((id, reason) in reasons) {
            val outcome = decoder.decode(dtoOf(id), alicePublic, bobPrivate)
            assertEquals("story $id", reason, (outcome as StoryDecoding.Skipped).reason)
        }
        // Inside alice's group but claiming carol as its author: opens, but is left out of the tray.
        assertFalse("5a000000-0000-4000-8000-000000000012" in shownIds)
        // Mallory is nobody's friend here: no group at all.
        assertTrue(decoded().authors.none { it.author.userId == ids.str("mallory") })
    }

    @Test
    fun aStoryThatIsKnownToBeSeenStaysSeen() {
        val seenFromServer = shown.filter { it.seen }.map { it.id }
        assertEquals(listOf("5a000000-0000-4000-8000-000000000002"), seenFromServer)
        val seenHere = decoded(setOf("5a000000-0000-4000-8000-000000000001")).authors.flatMap { it.stories }.filter { it.seen }.map { it.id }.toSet()
        assertEquals(setOf("5a000000-0000-4000-8000-000000000001", "5a000000-0000-4000-8000-000000000002"), seenHere)
    }

    @Test
    fun peopleWithSomethingNewComeFirst() {
        val allSeen = shown.map { it.id }.toSet()
        val tray = decoder.decodeFeed(feed(), bobPublic, bobPrivate, friends(), allSeen)
        assertTrue(tray.authors.all { it.allSeen })
        // Only the photo is seen from the server: carol's single story is the only one new, so she is first.
        val withCarolNew = decoder.decodeFeed(feed(), bobPublic, bobPrivate, friends(), shown.filter { it.authorId == ids.str("alice") }.map { it.id }.toSet())
        assertEquals(listOf("carol", "alice"), withCarolNew.authors.map { it.author.handle })
    }

    @Test
    fun anAuthorWhoseEveryStoryIsSkippedIsNotInTheTray() {
        val aliceId = ids.str("alice")
        val skippedOnly = feed().authors.single { it.authorId == aliceId }.stories.filter { it.kind == "audio" || it.kind == "reel" }
        assertEquals(2, skippedOnly.size)
        val tray = decoder.decodeFeed(StoryFeed(emptyList(), listOf(StoryAuthorGroup(aliceId, skippedOnly))), bobPublic, bobPrivate, friends(), emptySet())
        assertTrue(tray.authors.isEmpty())
    }

    @Test
    fun aPersonWhoIsNotAFriendIsNotShownEvenIfTheirStoriesWouldOpen() {
        val withoutAlice = friends() - ids.str("alice")
        val tray = decoder.decodeFeed(feed(), bobPublic, bobPrivate, withoutAlice, emptySet())
        assertEquals(listOf("carol"), tray.authors.map { it.author.handle })
        assertTrue(decoder.decodeFeed(feed(), bobPublic, bobPrivate, emptyMap(), emptySet()).authors.isEmpty())
    }

    @Test
    fun yourOwnStoriesOpenWithYourOwnKey() {
        val own = v.obj("ownFeed")
        val feed = StoryFeedParser.parse(own.obj("dto"))
        val alicePublic = keys.obj("alice").b64("publicKey")
        val alicePrivate = keys.obj("alice").b64("privateKey")
        val result = decoder.decodeFeed(feed, alicePublic, alicePrivate, emptyMap(), emptySet())
        val web = own.arr("webVisible").map { it.jsonObject }.single()
        val story = result.own.single()
        assertEquals(web.str("id"), story.id)
        assertArrayEquals(web.b64("key"), story.key)
        assertEquals(web.getValue("viewCount").jsonPrimitive.doubleOrNull!!.toInt(), story.viewCount)
        assertEquals("My own story", story.meta.text)
        assertTrue(story.seen)
    }

    @Test
    fun aFeedFromANewerServerIsStillRead() {
        // Unknown fields at every level (the vector has them) change nothing.
        assertEquals(3, feed().authors.size)
        // A story that can't be read at all (missing fields) is left out of its group, not the group.
        val original = v.obj("feedForBob")
        val group = original.arr("authors")[0].jsonObject
        val damaged = JsonArray(group.arr("stories") + buildJsonObject { put("id", 5) } + JsonPrimitive("garbage"))
        val patched = buildJsonObject {
            put("authors", JsonArray(listOf(JsonObject(group + ("stories" to damaged)))))
            put("own", JsonArray(listOf(JsonPrimitive(7))))
        }
        val parsed = StoryFeedParser.parse(patched)
        assertEquals(group.arr("stories").size, parsed.authors.single().stories.size)
        assertTrue(parsed.own.isEmpty())
        assertTrue(StoryFeedParser.parse(Json.parseToJsonElement("{}").jsonObject).authors.isEmpty())
    }

    // --- Media -----------------------------------------------------------------------------------------------

    private val fileCrypto = FileCrypto(sodium)

    private fun open(story: Story, mutate: (ByteArray) -> ByteArray = { it }, sha: String? = null, key: ByteArray? = null): ByteArray {
        val media = story.meta.media!!
        val vector = v.obj("media").obj(story.id)
        val out = ByteArrayOutputStream()
        fileCrypto.decrypt(
            ByteArrayInputStream(mutate(Base64Codec.decode(vector.str("ciphertext")))),
            out,
            key ?: Base64Codec.decode(media.key),
            Base64Codec.decode(media.header),
            media.encryptedSize,
            sha ?: media.sha256,
        )
        return out.toByteArray()
    }

    private fun sha256Hex(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }

    @Test
    fun aPhotoAndAVideoOpenWithTheKeyInTheirDetails() {
        for (story in shown.filter { it.hasMedia }) {
            val vector = v.obj("media").obj(story.id)
            val plain = open(story)
            assertEquals(vector.getValue("plaintextLength").jsonPrimitive.doubleOrNull!!.toInt(), plain.size)
            assertEquals(vector.str("plaintextSha256"), sha256Hex(plain))
            // The made-up file's own rule: byte i is (i * 7 + 3) mod 256.
            assertTrue(plain.indices.all { plain[it] == ((it * 7 + 3) and 0xff).toByte() })
        }
        // The photo is two chunks (70,000 bytes), so chunk framing is exercised too.
        val photo = shown.single { it.kind == StoryKind.Image && it.hasMedia }
        assertEquals(70_000L + 2 * 17, photo.meta.media!!.encryptedSize)
    }

    @Test
    fun mediaThatWasChangedCutOrReplacedIsRefused() {
        val photo = shown.single { it.kind == StoryKind.Image && it.hasMedia }
        val frame = 64 * 1024 + 17
        fun refused(label: String, block: () -> Unit) = assertThrows(label, FileDecryptException::class.java) { block() }
        refused("a changed byte") { open(photo, mutate = { it.copyOf().also { b -> b[100] = (b[100].toInt() xor 1).toByte() } }) }
        refused("a changed byte in the last chunk") { open(photo, mutate = { it.copyOf().also { b -> b[b.size - 1] = (b[b.size - 1].toInt() xor 1).toByte() } }) }
        refused("cut after the first chunk") { open(photo, mutate = { it.copyOf(frame) }) }
        refused("cut by one byte") { open(photo, mutate = { it.copyOf(it.size - 1) }) }
        refused("a byte added") { open(photo, mutate = { it + byteArrayOf(0) }) }
        refused("chunks swapped") { open(photo, mutate = { it.copyOfRange(frame, it.size) + it.copyOf(frame) }) }
        refused("the wrong digest") { open(photo, sha = "00".repeat(32)) }
        refused("the wrong key") { open(photo, key = ByteArray(32) { 1 }) }
    }

    // --- Settings ------------------------------------------------------------------------------------------------

    @Test
    fun settingsAreReadTheWayTheWebReadsThem() {
        val crypto = StoryCrypto(sodium)
        val prefs = v.obj("prefs")
        for (name in listOf("clean", "sloppy")) {
            val case = prefs.obj(name)
            val json = crypto.openStoryPrefs(case.b64("ciphertext"), case.b64("nonce"), bobPrivate)
            val read = StoryPrefsParser.parse(Json.parseToJsonElement(json))
            val web = case.obj("expected")
            assertEquals(name, web.arr("closeFriends").map { it.str() }, read.closeFriends.toList())
            assertEquals(name, web.arr("hideFrom").map { it.str() }, read.hideFrom.toList())
            assertEquals(name, web.arr("muted").map { it.str() }, read.muted.toList())
            assertEquals(name, web.getValue("shareViews").jsonPrimitive.booleanOrNull, read.shareViews)
            assertEquals(name, web.getValue("allowReplies").jsonPrimitive.booleanOrNull, read.allowReplies)
        }
    }

    @Test
    fun timestampsAreTheServersIsoSpelling() {
        // Instant.parse reads what the server's toISOString writes.
        assertEquals(1_792_497_600_000L, Instant.parse("2026-10-20T12:00:00.000Z").toEpochMilli())
    }
}
