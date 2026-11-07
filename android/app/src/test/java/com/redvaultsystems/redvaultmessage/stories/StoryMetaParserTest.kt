package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.data.model.MalformedStoryException
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryMetaParser
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefs
import com.redvaultsystems.redvaultmessage.data.model.StoryPrefsParser
import com.redvaultsystems.redvaultmessage.data.model.cleanStickerText
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The rules for reading a story's decrypted details (port of parseStoryMeta, ui/src/lib/storyCrypto.ts): unknown
 * values fall back, a wrong structure is refused. Needs no libsodium. The text-cleaning cases are the web's own
 * (story_interactive_vectors.json); the rest are written out here.
 */
class StoryMetaParserTest {
    private fun parse(json: String, kind: StoryKind) = StoryMetaParser.parse(Json.parseToJsonElement(json), kind)
    private fun refused(json: String, kind: StoryKind) {
        assertThrows(json, MalformedStoryException::class.java) { parse(json, kind) }
    }

    private val media = """{"key":"BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc=","header":"CQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJ","sha256":"${"ab".repeat(32)}","encryptedSize":65553,"chunkSize":65536,"mime":"image/jpeg","width":1080,"height":1920}"""

    @Test
    fun aTextStoryTakesItsLookFromTheDetailsAndFallsBackOnWhatItDoesNotKnow() {
        val meta = parse("""{"v":1,"kind":"text","text":"Hello","background":"aurora","font":"handwriting","overlays":[],"closeFriends":false}""", StoryKind.Text)
        assertEquals("Hello", meta.text)
        assertEquals(StoryBackground.Sunset, meta.background)
        assertEquals(StoryFont.Classic, meta.font)
        val known = parse("""{"v":1,"kind":"text","text":"Hi","background":"ocean","font":"typewriter","overlays":[]}""", StoryKind.Text)
        assertEquals(StoryBackground.Ocean, known.background)
        assertEquals(StoryFont.Typewriter, known.font)
        assertEquals(false, known.closeFriends)
    }

    @Test
    fun theStructureMustFit() {
        refused("""{"v":2,"kind":"text","text":"x","overlays":[]}""", StoryKind.Text)
        refused("""{"v":"1","kind":"text","text":"x","overlays":[]}""", StoryKind.Text)
        refused("""{"kind":"text","text":"x","overlays":[]}""", StoryKind.Text)
        refused("""{"v":1,"kind":"image","text":"x","overlays":[]}""", StoryKind.Text)
        refused("""{"v":1,"kind":"text","overlays":[]}""", StoryKind.Text)
        refused("""{"v":1,"kind":"text","text":"   ","overlays":[]}""", StoryKind.Text)
        refused("""{"v":1,"kind":"text","text":"${"x".repeat(701)}","overlays":[]}""", StoryKind.Text)
        // White space is JavaScript's: a byte order mark, a no-break space, a line separator and an ideographic space all count, so these are blank.
        for (blank in listOf("" + 0xFEFF.toChar(), "" + 0x00A0.toChar() + 0xFEFF.toChar(), "" + 0x2028.toChar() + 0x3000.toChar())) {
            val json = buildJsonObject { put("v", 1); put("kind", "text"); put("text", blank); put("overlays", JsonArray(emptyList())) }.toString()
            refused(json, StoryKind.Text)
        }
        refused("[]", StoryKind.Text)
        refused("\"text\"", StoryKind.Text)
        refused("null", StoryKind.Text)
        // 700 is the limit, and a version written as 1.0 is the number one.
        parse("""{"v":1.0,"kind":"text","text":"${"x".repeat(700)}"}""", StoryKind.Text)
    }

    @Test
    fun anOverlayThatIsWrongRefusesTheWholeStoryAsTheWebDoes() {
        val base = """{"v":1,"kind":"text","text":"x","overlays":[%s]}"""
        refused(base.format("""{"type":"banner","text":"a","x":0.5,"y":0.5}"""), StoryKind.Text)
        refused(base.format("""{"type":"text","text":"","x":0.5,"y":0.5}"""), StoryKind.Text)
        refused(base.format("""{"type":"text","text":"${"x".repeat(301)}","x":0.5,"y":0.5}"""), StoryKind.Text)
        refused(base.format("""{"type":"text","text":"a","x":1.5,"y":0.5}"""), StoryKind.Text)
        refused(base.format("""{"type":"text","text":"a","x":"0.5","y":0.5}"""), StoryKind.Text)
        refused(base.format("""{"type":"text","text":"a","x":0.5}"""), StoryKind.Text)
        refused(base.format("null"), StoryKind.Text)
        refused(base.format("""{"type":"text","text":"a","x":0.5,"y":0.5},3"""), StoryKind.Text)
    }

    @Test
    fun anOverlayWithOptionalFieldsItDoesNotKnowStillShows() {
        val meta = parse(
            """{"v":1,"kind":"text","text":"x","overlays":[
                {"type":"text","text":"a","x":0.2,"y":0.2,"scale":9,"rotation":-30,"color":"chartreuse","boxed":"yes","font":"gothic","anim":"spin","sticker3d":true},
                {"type":"emoji","text":"🙂","x":0.8,"y":0.8,"scale":0.1,"rotation":725,"anim":"none"},
                {"type":"text","text":"b","x":0,"y":1,"scale":"big","rotation":"spin","color":"teal","font":"strong","anim":"fade","boxed":true}
            ]}""",
            StoryKind.Text,
        )
        val (odd, emoji, third) = meta.overlays
        assertEquals(StoryColor.White, odd.color)
        assertEquals(StoryFont.Classic, odd.font)
        assertEquals(false, odd.boxed)
        assertEquals(4.0, odd.scale, 0.0) // clamped
        assertEquals(330.0, odd.rotation, 0.0) // -30 turned into 0..360
        assertNull(odd.anim) // an animation this app doesn't know: shown still
        assertTrue(emoji.isEmoji)
        assertEquals(0.5, emoji.scale, 0.0)
        assertEquals(5.0, emoji.rotation, 0.0)
        assertNull(emoji.anim)
        assertEquals(1.0, third.scale, 0.0) // not a number: the default
        assertEquals(0.0, third.rotation, 0.0)
        assertEquals(StoryColor.Teal, third.color)
        assertEquals("fade", third.anim?.wire) // known, kept, and not drawn
        assertEquals(true, third.boxed)
    }

    @Test
    fun onlyTheFirstThirtyOverlaysAreRead() {
        val overlay = """{"type":"text","text":"a","x":0.5,"y":0.5}"""
        val many = List(35) { overlay }.joinToString(",")
        assertEquals(30, parse("""{"v":1,"kind":"text","text":"x","overlays":[$many]}""", StoryKind.Text).overlays.size)
        // The cut comes before reading: a bad one past the thirtieth is never looked at.
        val bad = List(30) { overlay }.joinToString(",") + ",{\"type\":\"banner\"}"
        assertEquals(30, parse("""{"v":1,"kind":"text","text":"x","overlays":[$bad]}""", StoryKind.Text).overlays.size)
    }

    @Test
    fun aPhotoNeedsItsMediaToBeWellFormed() {
        val ok = parse("""{"v":1,"kind":"image","overlays":[],"media":$media}""", StoryKind.Image)
        assertEquals("image/jpeg", ok.media!!.mime)
        assertEquals(65553L, ok.media.encryptedSize)
        refused("""{"v":1,"kind":"image","overlays":[]}""", StoryKind.Image)
        refused("""{"v":1,"kind":"image","overlays":[],"media":${media.replace("image/jpeg", "image/gif")}}""", StoryKind.Image)
        refused("""{"v":1,"kind":"image","overlays":[],"media":${media.replace("image/jpeg", "video/mp4")}}""", StoryKind.Image)
        refused("""{"v":1,"kind":"image","overlays":[],"media":${media.replace("ab".repeat(32), "AB".repeat(32))}}""", StoryKind.Image)
        refused("""{"v":1,"kind":"image","overlays":[],"media":${media.replace("\"encryptedSize\":65553", "\"encryptedSize\":0")}}""", StoryKind.Image)
        refused("""{"v":1,"kind":"image","overlays":[],"media":${media.replace("\"encryptedSize\":65553", "\"encryptedSize\":1.5")}}""", StoryKind.Image)
        refused("""{"v":1,"kind":"image","overlays":[],"media":${media.replace("\"width\":1080", "\"width\":\"1080\"")}}""", StoryKind.Image)
        refused("""{"v":1,"kind":"image","overlays":[],"media":${media.replace("\"height\":1920", "\"height\":-1")}}""", StoryKind.Image)
        refused("""{"v":1,"kind":"image","overlays":[],"media":${media.replace("BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc=", "has space")}}""", StoryKind.Image)
        // A size written as 65553.0 is a whole number in JSON as in JavaScript.
        parse("""{"v":1,"kind":"image","overlays":[],"media":${media.replace("\"encryptedSize\":65553", "\"encryptedSize\":65553.0")}}""", StoryKind.Image)
    }

    @Test
    fun aVideoNeedsALengthAndItsCaptionsAreTidied() {
        val video = media.replace("image/jpeg", "video/mp4")
        refused("""{"v":1,"kind":"video","overlays":[],"media":$video}""", StoryKind.Video)
        val withLength = video.replace("\"height\":1920", "\"height\":1920,\"durationMs\":12000")
        val meta = parse(
            """{"v":1,"kind":"video","overlays":[],"media":$withLength,"captions":[
                {"startMs":1500,"endMs":3000,"text":"second"},
                {"startMs":0.4,"endMs":1400.5,"text":"  first   line "},
                {"startMs":5,"endMs":5,"text":"empty span"},
                {"startMs":-1,"endMs":10,"text":"before the start"},
                {"startMs":0,"endMs":600001,"text":"too long"},
                {"startMs":0,"endMs":10,"text":""},
                {"startMs":"0","endMs":10,"text":"string time"},
                "junk", null
            ]}""",
            StoryKind.Video,
        )
        assertEquals(12_000L, meta.media!!.durationMs)
        assertEquals(listOf("first line", "second"), meta.captions.map { it.text })
        assertEquals(0L, meta.captions[0].startMs)
        assertEquals(1401L, meta.captions[0].endMs) // Math.round(1400.5) is 1401
        assertEquals("first line", StoryMetaParser.captionAt(meta.captions, 1000))
        assertNull(StoryMetaParser.captionAt(meta.captions, 1401))
        assertEquals("second", StoryMetaParser.captionAt(meta.captions, 1500))
        assertNull(StoryMetaParser.captionAt(meta.captions, 3000))
    }

    @Test
    fun aPhotoIgnoresCaptionsAndAtMostSixtyAreRead() {
        val withCaptions = parse("""{"v":1,"kind":"image","overlays":[],"media":$media,"captions":[{"startMs":0,"endMs":10,"text":"x"}]}""", StoryKind.Image)
        assertTrue(withCaptions.captions.isEmpty())
        val lines = List(70) { """{"startMs":$it,"endMs":${it + 1},"text":"l$it"}""" }.joinToString(",")
        assertEquals(60, StoryMetaParser.parseCaptions(Json.parseToJsonElement("[$lines]")).size)
        assertTrue(StoryMetaParser.parseCaptions(null).isEmpty())
        assertTrue(StoryMetaParser.parseCaptions(Json.parseToJsonElement("{}")).isEmpty())
    }

    @Test
    fun closeFriendsIsOnlyAnExplicitTrue() {
        fun close(v: String) = parse("""{"v":1,"kind":"text","text":"x","overlays":[],"closeFriends":$v}""", StoryKind.Text).closeFriends
        assertEquals(true, close("true"))
        assertEquals(false, close("\"true\""))
        assertEquals(false, close("1"))
        assertEquals(false, close("null"))
    }

    @Test
    fun cleaningTextMatchesTheWeb() {
        val clean = vectors("story_interactive_vectors.json").obj("stickers").arr("clean")
        assertTrue(clean.size >= 20)
        for (case in clean) {
            val c = case as kotlinx.serialization.json.JsonObject
            val expected = c.getValue("expected").let { if (it is JsonNull) null else it.jsonPrimitive.content }
            assertEquals("input ${c.getValue("input")} max ${c.getValue("max")}", expected, cleanStickerText(c.getValue("input"), c.getValue("max").jsonPrimitive.content.toInt()))
        }
    }

    @Test
    fun settingsKeepOnlyRealIdsAndTreatEverythingButFalseAsOn() {
        val id = "00000000-0000-4000-8000-0000000000c3"
        val prefs = StoryPrefsParser.parse(
            Json.parseToJsonElement("""{"closeFriends":["${id.uppercase()}","$id","not-a-uuid",5,null],"hideFrom":["$id","$id"],"muted":"nobody","shareViews":"no","allowReplies":false,"new":{}}"""),
        )
        assertEquals(setOf(id), prefs.closeFriends)
        assertEquals(setOf(id), prefs.hideFrom)
        assertTrue(prefs.muted.isEmpty())
        assertEquals(true, prefs.shareViews)
        assertEquals(false, prefs.allowReplies)
        assertEquals(StoryPrefs.DEFAULT.shareViews, StoryPrefsParser.parse(Json.parseToJsonElement("{}")).shareViews)
        assertThrows(MalformedStoryException::class.java) { StoryPrefsParser.parse(Json.parseToJsonElement("[]")) }
    }
}
