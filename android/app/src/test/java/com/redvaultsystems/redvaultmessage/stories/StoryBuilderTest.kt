// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.data.PreparedStoryMedia
import com.redvaultsystems.redvaultmessage.data.StoryAudience
import com.redvaultsystems.redvaultmessage.data.StoryBuilder
import com.redvaultsystems.redvaultmessage.data.StoryDraft
import com.redvaultsystems.redvaultmessage.data.StoryFriend
import com.redvaultsystems.redvaultmessage.data.StoryInputException
import com.redvaultsystems.redvaultmessage.data.StoryPublishing
import com.redvaultsystems.redvaultmessage.data.model.LinkSticker
import com.redvaultsystems.redvaultmessage.data.model.PollSticker
import com.redvaultsystems.redvaultmessage.data.model.ResultsVisibility
import com.redvaultsystems.redvaultmessage.data.model.StoryAnimation
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryCaption
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.data.model.StoryOverlay
import com.redvaultsystems.redvaultmessage.data.model.StorySticker
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryMetaParser
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefs
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File

/**
 * What this app posts: who gets a key, what the request to the server looks like (its validation is strict), and that
 * what is sealed here opens again with the keys of the people it was sealed for. The story made at the end is written to
 * app/build/story-android-out/ and opened by the web app's own code (verify_android_posted.ts), the other half of the
 * compatibility check.
 */
class StoryBuilderTest {
    @get:Rule val tmp = TemporaryFolder()

    private val sodium = TestSodium.sodium
    private val crypto = StoryCrypto(sodium)
    private val fileCrypto = FileCrypto(sodium)
    private val builder = StoryBuilder(crypto, fileCrypto)
    private val v = vectors("story_wire_vectors.json")
    private val keys = v.obj("keys")
    private val ids = v.obj("ids")
    private val alice = keys.obj("alice")
    private val bobKeys = keys.obj("bob")
    private val carolKeys = keys.obj("carol")
    private val malloryKeys = keys.obj("mallory")

    private fun friend(name: String, k: kotlinx.serialization.json.JsonObject) = StoryFriend(ids.str(name), name, null, k.b64("publicKey"))
    private val bob = friend("bob", bobKeys)
    private val carol = friend("carol", carolKeys)
    private val mallory = friend("mallory", malloryKeys)
    private val friends = listOf(bob, carol, mallory)

    private val none = StoryPrefs.DEFAULT
    private val withLists = StoryPrefs(setOf(carol.userId), setOf(mallory.userId), emptySet(), shareViews = true, allowReplies = true)

    private fun draft(
        text: String? = "Hello from Android ☀️",
        media: PreparedStoryMedia? = null,
        audience: StoryAudience = StoryAudience.Friends,
        ttl: Int? = null,
        overlays: List<StoryOverlay> = emptyList(),
        stickers: List<StorySticker> = emptyList(),
        customIds: Set<String> = emptySet(),
        captions: List<StoryCaption> = emptyList(),
    ) = StoryDraft(text, StoryBackground.Ocean, StoryFont.Typewriter, media, audience, allowReplies = true, ttlSeconds = ttl, overlays = overlays, stickers = stickers, customIds = customIds, captions = captions)

    private fun build(d: StoryDraft, prefs: StoryPrefs = none, who: Collection<StoryFriend> = friends) =
        builder.build(ids.str("alice"), alice.b64("publicKey"), alice.b64("privateKey"), d, who, prefs, File(tmp.root, "upload"))

    private fun fileOf(bytes: ByteArray, kind: StoryKind, mime: String, durationMs: Long? = null) =
        PreparedStoryMedia(kind, mime, 640, 480, durationMs, bytes.size.toLong()) { ByteArrayInputStream(bytes) }

    private fun keyUserIds(body: JsonObject) = body.arr("keys").map { it.jsonObject.str("userId") }

    @Test
    fun recipientsAreFriendsMinusTheHiddenAndCloseFriendsOnlyForThose() {
        assertEquals(listOf(bob.userId, carol.userId, mallory.userId), StoryPublishing.recipients(friends, none, StoryAudience.Friends).map { it.userId })
        assertEquals(listOf(bob.userId, carol.userId), StoryPublishing.recipients(friends, withLists, StoryAudience.Friends).map { it.userId })
        assertEquals(listOf(carol.userId), StoryPublishing.recipients(friends, withLists, StoryAudience.CloseFriends).map { it.userId })
        assertTrue(StoryPublishing.recipients(friends, none, StoryAudience.CloseFriends).isEmpty())
        // Hiding wins over close friends.
        val both = StoryPrefs(setOf(carol.userId), setOf(carol.userId), emptySet(), true, true)
        assertTrue(StoryPublishing.recipients(friends, both, StoryAudience.CloseFriends).isEmpty())
        assertTrue(StoryPublishing.recipients(emptyList(), none, StoryAudience.Friends).isEmpty())
    }

    @Test
    fun aTextStoryGoesToYouAndToTheRightFriendsAndTheyAllOpenIt() {
        val built = build(draft(), withLists)
        val body = built.body
        assertEquals(listOf(ids.str("alice"), bob.userId, carol.userId), keyUserIds(body)) // mallory is hidden, you come first
        assertEquals(setOf("id", "kind", "metaCiphertext", "metaNonce", "mediaSize", "audience", "allowReplies", "keys"), body.keys)
        assertEquals(JsonNull, body["mediaSize"]) // spelled out: the server's validation insists
        assertEquals("text", body.str("kind"))
        assertEquals("friends", body.str("audience"))
        assertNull(built.encryptedMedia)
        assertTrue(Regex("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$").matches(built.id))
        val ciphertext = body.b64("metaCiphertext")
        val nonce = body.b64("metaNonce")
        val recipients = mapOf(ids.str("alice") to alice, bob.userId to bobKeys, carol.userId to carolKeys)
        for (entry in body.arr("keys").map { it.jsonObject }) {
            val me = recipients.getValue(entry.str("userId"))
            val wrapped = Base64Codec.decode(entry.str("wrappedKey"))
            assertEquals(93, wrapped.size)
            val key = crypto.unwrapStoryKey(built.id, wrapped, alice.b64("publicKey"), me.b64("privateKey"))
            val meta = StoryMetaParser.parse(Json.parseToJsonElement(crypto.openStoryMeta(built.id, key, ciphertext, nonce)), StoryKind.Text)
            assertEquals("Hello from Android ☀️", meta.text)
            assertEquals(StoryBackground.Ocean, meta.background)
            assertEquals(StoryFont.Typewriter, meta.font)
            assertFalse(meta.closeFriends)
        }
        // Someone who got no key cannot open it: the hidden friend's own key does not fit.
        assertTrue(runCatching {
            crypto.unwrapStoryKey(built.id, Base64Codec.decode(body.arr("keys")[1].jsonObject.str("wrappedKey")), alice.b64("publicKey"), malloryKeys.b64("privateKey"))
        }.isFailure)
    }

    @Test
    fun aCloseFriendsStoryIsMarkedAndGoesOnlyToThem() {
        val built = build(draft(audience = StoryAudience.CloseFriends), withLists)
        assertEquals(listOf(ids.str("alice"), carol.userId), keyUserIds(built.body))
        assertEquals("close_friends", built.body.str("audience"))
        val key = crypto.unwrapStoryKey(built.id, Base64Codec.decode(built.body.arr("keys")[1].jsonObject.str("wrappedKey")), alice.b64("publicKey"), carolKeys.b64("privateKey"))
        val meta = StoryMetaParser.parse(Json.parseToJsonElement(crypto.openStoryMeta(built.id, key, built.body.b64("metaCiphertext"), built.body.b64("metaNonce"))), StoryKind.Text)
        assertTrue(meta.closeFriends)
        assertThrows(StoryInputException::class.java) { build(draft(audience = StoryAudience.CloseFriends), none) }
    }

    private fun metaOf(built: com.redvaultsystems.redvaultmessage.data.BuiltStory, kind: StoryKind): com.redvaultsystems.redvaultmessage.data.model.StoryMeta {
        val key = crypto.unwrapStoryKey(built.id, Base64Codec.decode(built.body.arr("keys")[0].jsonObject.str("wrappedKey")), alice.b64("publicKey"), alice.b64("privateKey"))
        return StoryMetaParser.parse(Json.parseToJsonElement(crypto.openStoryMeta(built.id, key, built.body.b64("metaCiphertext"), built.body.b64("metaNonce"))), kind)
    }

    @Test
    fun aStoryForPickedFriendsGoesOnlyToThemNeverToTheHiddenOrToStrangers() {
        val picked = setOf(bob.userId, mallory.userId, "00000000-0000-4000-8000-0000000000ff")
        assertEquals(listOf(bob.userId), StoryPublishing.recipients(friends, withLists, StoryAudience.Custom, picked).map { it.userId })
        assertTrue(StoryPublishing.recipients(friends, none, StoryAudience.Custom).isEmpty())
        val built = build(draft(audience = StoryAudience.Custom, customIds = picked), withLists)
        assertEquals(listOf(ids.str("alice"), bob.userId), keyUserIds(built.body)) // mallory is hidden, the stranger is no friend
        assertEquals("custom", built.body.str("audience"))
        val key = crypto.unwrapStoryKey(built.id, Base64Codec.decode(built.body.arr("keys")[1].jsonObject.str("wrappedKey")), alice.b64("publicKey"), bobKeys.b64("privateKey"))
        val meta = StoryMetaParser.parse(Json.parseToJsonElement(crypto.openStoryMeta(built.id, key, built.body.b64("metaCiphertext"), built.body.b64("metaNonce"))), StoryKind.Text)
        assertFalse("not marked close friends", meta.closeFriends)
        val refused = assertThrows(StoryInputException::class.java) { build(draft(audience = StoryAudience.Custom, customIds = setOf(mallory.userId)), withLists) }
        assertEquals("Choose at least one friend.", refused.message)
    }

    @Test
    fun wordsEmojiAndStickersAreSealedWithTheStoryAndReadBackAsPlaced() {
        val words = StoryOverlay(false, "Shh", 0.3, 0.2, 1.4, 10.0, StoryColor.Yellow, true, StoryFont.Strong, StoryAnimation.Pulse)
        val emoji = StoryOverlay(true, "🤫", 0.7, 0.25, 1.1, 0.0, StoryColor.White, false, StoryFont.Classic, null)
        val poll = PollSticker("pollsticker1", 0.4, 0.6, 1.2, 5.0, "Secret plan? 🎉", listOf("Yes", "No"), 1_900_000_000_000, ResultsVisibility.Everyone)
        val link = LinkSticker("linksticker1", 0.5, 0.85, 1.0, 0.0, "https://example.com/party?x=1", "Details")
        val built = build(draft(overlays = listOf(words, emoji), stickers = listOf(poll, link)))
        val meta = metaOf(built, StoryKind.Text)
        assertEquals(listOf(words, emoji), meta.overlays)
        assertEquals(listOf<StorySticker>(poll, link), meta.stickers)
        // Nothing of it is in the request but ciphertext.
        assertFalse(built.body.toString().contains("Secret plan"))
        assertFalse(built.body.toString().contains("example.com"))
        // A story with no stickers carries no stickers field at all, as the web writes it.
        val plain = build(draft())
        val plainKey = crypto.unwrapStoryKey(plain.id, Base64Codec.decode(plain.body.arr("keys")[0].jsonObject.str("wrappedKey")), alice.b64("publicKey"), alice.b64("privateKey"))
        assertFalse("stickers" in Json.parseToJsonElement(crypto.openStoryMeta(plain.id, plainKey, plain.body.b64("metaCiphertext"), plain.body.b64("metaNonce"))).jsonObject)
    }

    @Test
    fun aVideoKeepsItsSubtitlesAndAPhotoDoesNotGetAny() {
        val captions = listOf(StoryCaption(0, 1_500, "first"), StoryCaption(2_000, 4_000, "second"))
        val video = metaOf(build(draft(text = null, media = fileOf(ByteArray(1000) { 5 }, StoryKind.Video, "video/mp4", 5_000), captions = captions)), StoryKind.Video)
        assertEquals(listOf("first", "second"), video.captions.map { it.text })
        assertEquals(listOf(0L to 1_500L, 2_000L to 4_000L), video.captions.map { it.startMs to it.endMs })
        val photo = metaOf(build(draft(text = null, media = fileOf(ByteArray(1000) { 5 }, StoryKind.Image, "image/jpeg"), captions = captions)), StoryKind.Image)
        assertTrue(photo.captions.isEmpty())
        // The parser would ignore subtitles on a photo, so look at what is actually sealed.
        fun sealed(built: com.redvaultsystems.redvaultmessage.data.BuiltStory): JsonObject {
            val key = crypto.unwrapStoryKey(built.id, Base64Codec.decode(built.body.arr("keys")[0].jsonObject.str("wrappedKey")), alice.b64("publicKey"), alice.b64("privateKey"))
            return Json.parseToJsonElement(crypto.openStoryMeta(built.id, key, built.body.b64("metaCiphertext"), built.body.b64("metaNonce"))).jsonObject
        }
        assertFalse("captions" in sealed(build(draft(text = null, media = fileOf(ByteArray(1000) { 5 }, StoryKind.Image, "image/jpeg"), captions = captions))))
        assertEquals(2, sealed(build(draft(text = null, media = fileOf(ByteArray(1000) { 5 }, StoryKind.Video, "video/mp4", 5_000), captions = captions))).arr("captions").size)
        assertFalse("none given, none written", "captions" in sealed(build(draft(text = null, media = fileOf(ByteArray(1000) { 5 }, StoryKind.Video, "video/mp4", 5_000)))))
    }

    @Test
    fun aStoryWithNoFriendsStillGoesToYou() {
        val built = build(draft(), none, emptyList())
        assertEquals(listOf(ids.str("alice")), keyUserIds(built.body))
    }

    @Test
    fun theLifetimeIsOnlySentWhenChosen() {
        assertFalse("ttlSeconds" in build(draft()).body)
        assertEquals(21_600, build(draft(ttl = 21_600)).body["ttlSeconds"]!!.jsonPrimitive.content.toInt())
    }

    @Test
    fun textIsTidiedAndLimited() {
        val padded = build(draft(text = "\u00A0\uFEFF  words \n"))
        val key = crypto.unwrapStoryKey(padded.id, Base64Codec.decode(padded.body.arr("keys")[0].jsonObject.str("wrappedKey")), alice.b64("publicKey"), alice.b64("privateKey"))
        val meta = StoryMetaParser.parse(Json.parseToJsonElement(crypto.openStoryMeta(padded.id, key, padded.body.b64("metaCiphertext"), padded.body.b64("metaNonce"))), StoryKind.Text)
        assertEquals("words", meta.text)
        assertThrows("blank", StoryInputException::class.java) { build(draft(text = " \u00A0 ")) }
        assertThrows("empty", StoryInputException::class.java) { build(draft(text = "")) }
        assertThrows("null", StoryInputException::class.java) { build(draft(text = null)) }
        assertThrows("too long", StoryInputException::class.java) { build(draft(text = "x".repeat(701))) }
        build(draft(text = "x".repeat(700)))
    }

    @Test
    fun aPhotoIsEncryptedWithAKeyOnlyTheSealedDetailsHold() {
        val plain = ByteArray(70_000) { ((it * 7 + 3) and 0xff).toByte() }
        val built = build(draft(text = null, media = fileOf(plain, StoryKind.Image, "image/jpeg")), withLists)
        val file = built.encryptedMedia!!
        assertEquals("image", built.body.str("kind"))
        assertEquals(file.length(), built.body["mediaSize"]!!.jsonPrimitive.content.toLong())
        assertEquals(70_000L + 2 * 17, file.length())
        val key = crypto.unwrapStoryKey(built.id, Base64Codec.decode(built.body.arr("keys")[1].jsonObject.str("wrappedKey")), alice.b64("publicKey"), bobKeys.b64("privateKey"))
        val details = crypto.openStoryMeta(built.id, key, built.body.b64("metaCiphertext"), built.body.b64("metaNonce"))
        val meta = StoryMetaParser.parse(Json.parseToJsonElement(details), StoryKind.Image)
        val media = meta.media!!
        assertEquals("image/jpeg", media.mime)
        assertEquals(640, media.width)
        assertEquals(65536L, media.chunkSize)
        val out = ByteArrayOutputStream()
        file.inputStream().use { fileCrypto.decrypt(it, out, Base64Codec.decode(media.key), Base64Codec.decode(media.header), media.encryptedSize, media.sha256) }
        assertArrayEquals(plain, out.toByteArray())
        // The server only ever sees the ciphertext and these ids: nothing in the request carries the file key.
        assertFalse(built.body.toString().contains(media.key))
        assertFalse(built.body.toString().contains("image/jpeg"))
    }

    @Test
    fun aVideoCarriesItsLengthAndAFailedBuildLeavesNoFileBehind() {
        val video = build(draft(text = null, media = fileOf(ByteArray(1000) { 5 }, StoryKind.Video, "video/mp4", 12_000)))
        val key = crypto.unwrapStoryKey(video.id, Base64Codec.decode(video.body.arr("keys")[0].jsonObject.str("wrappedKey")), alice.b64("publicKey"), alice.b64("privateKey"))
        val meta = StoryMetaParser.parse(Json.parseToJsonElement(crypto.openStoryMeta(video.id, key, video.body.b64("metaCiphertext"), video.body.b64("metaNonce"))), StoryKind.Video)
        assertEquals(12_000L, meta.media!!.durationMs)
        val failing = File(tmp.root, "failing")
        assertThrows(StoryInputException::class.java) {
            builder.build(ids.str("alice"), alice.b64("publicKey"), alice.b64("privateKey"), draft(text = null, media = fileOf(ByteArray(10), StoryKind.Image, "image/jpeg"), audience = StoryAudience.CloseFriends), friends, none, failing)
        }
        assertFalse(failing.exists())
    }

    @Test
    fun writeAStoryOfEachKindForTheWebToOpen() {
        // Fixed inputs; the output is only used by verify_android_posted.ts.
        val photoBytes = ByteArray(70_000) { ((it * 7 + 3) and 0xff).toByte() }
        val videoBytes = ByteArray(2_000) { ((it * 11 + 1) and 0xff).toByte() }
        val drafts = listOf(
            "text" to draft(),
            "close" to draft(text = "Close friends only", audience = StoryAudience.CloseFriends),
            "photo" to draft(text = null, media = fileOf(photoBytes, StoryKind.Image, "image/jpeg")),
            "video" to draft(text = null, media = fileOf(videoBytes, StoryKind.Video, "video/mp4", 9_000)),
            "ttl" to draft(text = "Six hours", ttl = 21_600),
            "decorated" to draft(
                text = "Decorated",
                overlays = listOf(
                    StoryOverlay(false, "Shh", 0.3, 0.2, 1.4, 10.0, StoryColor.Yellow, true, StoryFont.Strong, StoryAnimation.Pulse),
                    StoryOverlay(true, "🤫", 0.7, 0.25, 1.1, 0.0, StoryColor.White, false, StoryFont.Classic, null),
                ),
                stickers = listOf(
                    PollSticker("pollsticker1", 0.4, 0.6, 1.2, 5.0, "Secret plan? 🎉", listOf("Yes", "No"), 1_900_000_000_000, ResultsVisibility.Everyone),
                    LinkSticker("linksticker1", 0.5, 0.85, 1.0, 0.0, "https://example.com/party?x=1", "Details"),
                ),
            ),
            "picked" to draft(text = "Only for Bob", audience = StoryAudience.Custom, customIds = setOf(bob.userId)),
            "captioned" to draft(text = null, media = fileOf(videoBytes, StoryKind.Video, "video/mp4", 9_000), captions = listOf(StoryCaption(0, 1_500, "first"), StoryCaption(2_000, 4_000, "second"))),
        )
        val stories = buildJsonArray {
            for ((name, d) in drafts) {
                val built = builder.build(ids.str("alice"), alice.b64("publicKey"), alice.b64("privateKey"), d, friends, withLists, File(tmp.root, "up-$name"))
                fun keyOf(userId: String) = built.body.arr("keys").map { it.jsonObject }.firstOrNull { it.str("userId") == userId }?.str("wrappedKey")
                val bobKey = keyOf(bob.userId)
                val carolKey = keyOf(carol.userId)
                add(
                    buildJsonObject {
                        put("name", name)
                        put("id", built.id)
                        put("authorId", ids.str("alice"))
                        put("kind", built.body.str("kind"))
                        put("metaCiphertext", built.body.str("metaCiphertext"))
                        put("metaNonce", built.body.str("metaNonce"))
                        put("mediaSize", built.body.getValue("mediaSize"))
                        put("hasMedia", built.encryptedMedia != null)
                        put("allowReplies", true)
                        put("audience", built.body.str("audience"))
                        put("wrappedKeyForBob", bobKey?.let { JsonPrimitive(it) } ?: JsonNull)
                        put("wrappedKeyForCarol", carolKey?.let { JsonPrimitive(it) } ?: JsonNull)
                        put("ttlSeconds", built.body["ttlSeconds"] ?: JsonNull)
                        put("recipients", JsonArray(built.body.arr("keys").map { it.jsonObject.getValue("userId") }))
                        put("media", built.encryptedMedia?.let { JsonPrimitive(Base64Codec.encode(it.readBytes())) } ?: JsonNull)
                    },
                )
            }
        }
        val out = File("build/story-android-out").apply { mkdirs() }
        File(out, "android_posted.json").writeText(
            buildJsonObject {
                put("note", "Made by StoryBuilderTest with this app's own story code; opened by verify_android_posted.ts with the web app's.")
                put("ids", ids)
                put("keys", keys)
                put("stories", stories)
                put("photoLength", photoBytes.size)
                put("videoLength", videoBytes.size)
            }.toString(),
        )
        assertTrue(File(out, "android_posted.json").length() > 1000)
        assertEquals(8, stories.size)
    }
}
