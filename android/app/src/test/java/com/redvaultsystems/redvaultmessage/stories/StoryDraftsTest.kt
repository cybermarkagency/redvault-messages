// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.data.DraftContent
import com.redvaultsystems.redvaultmessage.data.DraftSlide
import com.redvaultsystems.redvaultmessage.data.PreparedStoryMedia
import com.redvaultsystems.redvaultmessage.data.SlideMode
import com.redvaultsystems.redvaultmessage.data.StoryAudience
import com.redvaultsystems.redvaultmessage.data.StoryDraftStore
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryStickerParser
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
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
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.File

/**
 * Unfinished stories kept on the phone. The vectors are drafts the web app's own code sealed (version 2, several slides): they open
 * here, and what is sealed here carries the same key derivation and binding. Nothing readable is on disk.
 */
class StoryDraftsTest {
    @get:Rule val tmp = TemporaryFolder()

    private val sodium = TestSodium.sodium
    private val crypto = StoryCrypto(sodium)
    private val v = vectors("story_draft_v2_vectors.json")
    private val userId = v.str("userId")
    private val me = SelfIdentity("me", userId, v.b64("publicKey"), v.b64("privateKey"), null, "user", "free", false)
    private val other = SelfIdentity("other", v.str("otherUserId"), ByteArray(32), v.b64("otherPrivateKey"), null, "user", "free", false)

    private fun store(now: Long = System.currentTimeMillis()) =
        StoryDraftStore(File(tmp.root, "drafts"), File(tmp.root, "work"), crypto, FileCrypto(sodium)) { now }

    private val drafts = v.arr("drafts").map { it.jsonObject }

    /** Lays a web-made draft out the way the store keeps them. */
    private fun place(draft: JsonObject, forUser: String = userId, id: String = draft.str("id")) {
        val dir = File(File(tmp.root, "drafts"), forUser.lowercase()).also { it.mkdirs() }
        val file = File(dir, "$id.draft")
        file.writeBytes(draft.b64("nonce") + draft.b64("sealed"))
        file.setLastModified(draft.getValue("updatedAt").jsonPrimitive.content.toLong())
        draft.arr("media").forEachIndexed { i, m -> if (m !is JsonNull) File(dir, "$id.m$i").writeBytes(Base64Codec.decode(m.jsonPrimitive.content)) }
    }

    private fun canon(e: JsonElement): Any? = when (e) {
        is JsonNull -> null
        is JsonObject -> e.entries.sortedBy { it.key }.associate { it.key to canon(it.value) }
        is JsonArray -> e.map(::canon)
        is JsonPrimitive -> if (e.isString) e.content else (e.doubleOrNull ?: e.content)
    }

    @Test
    fun draftsTheWebSealedAreListedAndOpened() {
        for (draft in drafts) place(draft)
        val at = drafts.maxOf { it.getValue("updatedAt").jsonPrimitive.content.toLong() } + 3600_000
        val s = store(at)
        val listed = s.list(me)
        assertEquals(drafts.size, listed.size)
        for (draft in drafts) {
            val summary = listed.single { it.id == draft.str("id") }
            val web = draft.obj("summary")
            assertEquals(web.getValue("slideCount").jsonPrimitive.content.toInt(), summary.slideCount)
            assertEquals(web.str("preview"), summary.preview)
            assertEquals(web.getValue("stickerCount").jsonPrimitive.content.toInt(), summary.stickerCount)

            val content = s.load(me, draft.str("id"))!!
            val expected = draft.obj("expected")
            assertEquals(expected.str("audience"), content.audience.wire)
            assertEquals(expected.arr("customIds").map { it.jsonPrimitive.content }, content.customIds)
            assertEquals(expected.getValue("allowReplies").jsonPrimitive.content == "true", content.allowReplies)
            assertEquals(expected.getValue("ttlSeconds").jsonPrimitive.content.toInt(), content.ttlSeconds)
            val webSlides = expected.arr("slides").map { it.jsonObject }
            assertEquals(webSlides.size, content.slides.size)
            webSlides.zip(content.slides).forEachIndexed { i, (web, slide) ->
                val at = "${draft.str("name")} slide $i"
                assertEquals(at, web.str("mode"), slide.mode.wire)
                assertEquals(at, web.str("text"), slide.text)
                assertEquals(at, web.str("background"), slide.background.wire)
                assertEquals(at, web.str("font"), slide.font.wire)
                assertEquals(at, web.arr("overlays").size, slide.overlays.size)
                web.arr("overlays").map { it.jsonObject }.zip(slide.overlays).forEach { (w, o) ->
                    assertEquals(w.str("text"), o.text)
                    assertEquals(w.str("type"), if (o.isEmoji) "emoji" else "text")
                    assertEquals(w.str("color"), o.color.wire)
                    assertEquals(w.getValue("boxed").jsonPrimitive.content == "true", o.boxed)
                    assertEquals(w["anim"]?.jsonPrimitive?.content, o.anim?.wire)
                }
                assertEquals(at, web.arr("stickers").map(::canon), slide.stickers.map { canon(StoryStickerParser.toJson(it)) })
                assertEquals(at, web.arr("captions").size, slide.captions.size)
                val webMedia = web["media"]
                if (webMedia == null || webMedia is JsonNull) {
                    assertNull(at, slide.media)
                } else {
                    val m = slide.media!!
                    val wm = webMedia.jsonObject
                    assertEquals(wm.str("kind"), m.kind.wire)
                    assertEquals(wm.str("mime"), m.mime)
                    assertEquals(wm.getValue("width").jsonPrimitive.content.toInt(), m.width)
                    assertEquals(wm["durationMs"]?.jsonPrimitive?.content?.toLong(), m.durationMs)
                    assertArrayEquals(at, Base64Codec.decode(wm.str("bytes")), m.open().use { it.readBytes() })
                }
            }
        }
    }

    @Test
    fun theKeyAndBindingAreTheWebs() {
        // Sealing the web's own plaintext with the web's own nonce gives the web's own bytes: same key derivation, same additional data.
        for (draft in drafts) {
            val json = crypto.openDraft(draft.b64("sealed"), draft.b64("nonce"), userId, draft.str("id"), me.privateKey)!!
            assertTrue(json.startsWith("{\"v\":2,"))
            assertArrayEquals(draft.b64("sealed"), crypto.sealDraft(json, userId, draft.str("id"), me.privateKey, draft.b64("nonce")).ciphertext)
            // Another account's key, another draft's id, another account's id: nothing.
            assertNull(crypto.openDraft(draft.b64("sealed"), draft.b64("nonce"), userId, draft.str("id"), other.privateKey))
            assertNull(crypto.openDraft(draft.b64("sealed"), draft.b64("nonce"), userId, "ffffffffffffffffffffffff", me.privateKey))
            assertNull(crypto.openDraft(draft.b64("sealed"), draft.b64("nonce"), other.userId, draft.str("id"), me.privateKey))
        }
    }

    @Test
    fun aDraftMovedToAnotherAccountOrChangedIsDroppedNotShown() {
        val d = drafts.first()
        place(d, forUser = other.userId) // the file is under another account, sealed for the first
        val at = d.getValue("updatedAt").jsonPrimitive.content.toLong() + 1000
        val s = store(at)
        assertTrue(s.list(other).isEmpty())
        assertFalse(File(File(File(tmp.root, "drafts"), other.userId.lowercase()), d.str("id") + ".draft").exists())
        // Renamed to another draft id: it no longer opens.
        place(d, id = "aaaaaaaaaaaaaaaaaaaaaaaa")
        assertTrue(s.list(me).none { it.id == "aaaaaaaaaaaaaaaaaaaaaaaa" })
        // A changed byte.
        place(d)
        val file = File(File(File(tmp.root, "drafts"), userId.lowercase()), d.str("id") + ".draft")
        val bytes = file.readBytes()
        bytes[bytes.size - 1] = (bytes[bytes.size - 1].toInt() xor 1).toByte()
        file.writeBytes(bytes)
        file.setLastModified(at - 1000)
        assertTrue(s.list(me).none { it.id == d.str("id") })
    }

    @Test
    fun aDraftOfAnotherFormatVersionIsDroppedNotShown() {
        fun layOut(id: String, version: Int?) {
            val json = buildString {
                append("{")
                if (version != null) append("\"v\":$version,")
                append("\"story\":{\"audience\":\"friends\",\"customIds\":[],\"allowReplies\":true,\"ttlSeconds\":86400},")
                append("\"slides\":[{\"content\":{\"mode\":\"text\",\"text\":\"x\",\"background\":\"sunset\",\"font\":\"classic\",\"overlays\":[],\"stickers\":[],\"captions\":[]},\"media\":null}],")
                append("\"summary\":{\"slideCount\":1,\"preview\":\"x\",\"stickerCount\":0}}")
            }
            val sealed = crypto.sealDraft(json, userId, id, me.privateKey)
            val dir = File(File(tmp.root, "drafts"), userId.lowercase()).also { it.mkdirs() }
            File(dir, "$id.draft").writeBytes(sealed.nonce + sealed.ciphertext)
        }
        layOut("good", 2)
        layOut("later", 3)
        layOut("older", 1)
        layOut("unnumbered", null)
        val s = store()
        assertEquals(listOf("good"), s.list(me).map { it.id })
        assertNotNull(s.load(me, "good"))
        for (id in listOf("later", "older", "unnumbered")) {
            assertNull(id, s.load(me, id))
            assertFalse("$id removed", File(File(File(tmp.root, "drafts"), userId.lowercase()), "$id.draft").exists())
        }
    }

    @Test
    fun aDraftOlderThanThirtyDaysIsDeleted() {
        val d = drafts.first()
        place(d)
        val updated = d.getValue("updatedAt").jsonPrimitive.content.toLong()
        assertEquals(1, store(updated + 29L * 24 * 3600_000).list(me).size)
        assertEquals(0, store(updated + 31L * 24 * 3600_000).list(me).size)
        assertFalse(File(File(File(tmp.root, "drafts"), userId.lowercase()), d.str("id") + ".draft").exists())
    }

    private fun media(bytes: ByteArray, kind: StoryKind, mime: String, durationMs: Long? = null) =
        PreparedStoryMedia(kind, mime, 640, 480, durationMs, bytes.size.toLong()) { ByteArrayInputStream(bytes) }

    private fun slide(text: String = "", m: PreparedStoryMedia? = null) =
        DraftSlide(if (m != null) SlideMode.Media else SlideMode.Text, text, StoryBackground.Berry, StoryFont.Modern, emptyList(), emptyList(), emptyList(), m)

    @Test
    fun aDraftSavedHereComesBackAndLeavesNothingReadableOnDisk() {
        val s = store()
        val photo = ByteArray(70_000) { ((it * 7 + 3) and 0xff).toByte() }
        val secret = "a very private surprise"
        val id = s.save(me, DraftContent(listOf(slide(secret), slide(m = media(photo, StoryKind.Image, "image/jpeg"))), StoryAudience.Custom, listOf("00000000-0000-4000-8000-00000000000c"), allowReplies = false, ttlSeconds = 21_600))
        val listed = s.list(me).single()
        assertEquals(id, listed.id)
        assertEquals(2, listed.slideCount)
        assertEquals(secret, listed.preview)
        val back = s.load(me, id)!!
        assertEquals(secret, back.slides[0].text)
        assertEquals(StoryBackground.Berry, back.slides[0].background)
        assertEquals(StoryAudience.Custom, back.audience)
        assertEquals(false, back.allowReplies)
        assertEquals(21_600, back.ttlSeconds)
        assertArrayEquals(photo, back.slides[1].media!!.open().use { it.readBytes() })
        // On disk: the words and the picture's bytes appear nowhere.
        for (file in File(File(tmp.root, "drafts"), userId.lowercase()).listFiles().orEmpty()) {
            val bytes = file.readBytes()
            assertFalse(file.name, String(bytes, Charsets.ISO_8859_1).contains(secret))
            assertFalse(file.name, String(bytes, Charsets.ISO_8859_1).contains("redvault"))
            assertFalse(file.name, String(bytes, Charsets.ISO_8859_1).contains("closeFriends"))
        }
        assertFalse("the photo's bytes", File(File(tmp.root, "drafts"), userId.lowercase()).listFiles().orEmpty().any { f -> f.readBytes().toList().windowed(32).any { w -> w == photo.toList().subList(1000, 1032) } })
        // Another account on the same phone sees nothing.
        assertTrue(s.list(other).isEmpty())
        assertNull(s.load(other, id))
        // Deleting removes the draft and its media.
        s.delete(me, id)
        assertTrue(File(File(tmp.root, "drafts"), userId.lowercase()).listFiles().orEmpty().isEmpty())
        assertNull(s.load(me, id))
    }

    @Test
    fun savingAgainReplacesTheSameDraftAndTenIsTheMost() {
        var now = 1_800_000_000_000L
        val s = StoryDraftStore(File(tmp.root, "drafts"), File(tmp.root, "work"), crypto, FileCrypto(sodium)) { now }
        val first = s.save(me, DraftContent(listOf(slide("one")), StoryAudience.Friends, emptyList(), true, 86_400))
        now += 1000
        assertEquals(first, s.save(me, DraftContent(listOf(slide("one, edited")), StoryAudience.Friends, emptyList(), true, 86_400), first))
        assertEquals(1, s.list(me).size)
        assertEquals("one, edited", s.list(me).single().preview)
        repeat(12) {
            now += 1000
            s.save(me, DraftContent(listOf(slide("draft $it")), StoryAudience.Friends, emptyList(), true, 86_400))
        }
        val all = s.list(me)
        assertEquals(10, all.size)
        // The oldest went first, so the first one is gone and the newest is listed first.
        assertTrue(all.none { it.id == first })
        assertEquals("draft 11", all.first().preview)
        assertNotNull(s.load(me, all.first().id))
    }
}
