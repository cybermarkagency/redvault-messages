// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.data.SlideMode
import com.redvaultsystems.redvaultmessage.data.model.StoryAnimation
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryOverlay
import com.redvaultsystems.redvaultmessage.data.model.StoryStickerParser
import com.redvaultsystems.redvaultmessage.studio.MotionFrame
import com.redvaultsystems.redvaultmessage.studio.StoryCollage
import com.redvaultsystems.redvaultmessage.studio.StoryMotion
import com.redvaultsystems.redvaultmessage.studio.TextTemplates
import com.redvaultsystems.redvaultmessage.ui.stories.ComposerModel
import com.redvaultsystems.redvaultmessage.ui.stories.Selected
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.double
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Collages, text templates and animated words: the layouts, the maths and the template contents against what the web's own code makes of the
 * same inputs (studio_vectors.json), and the motion curves against the key moments of the web's CSS animations.
 */
class CollageTemplatesMotionTest {
    private val v = vectors("studio_vectors.json")
    private val collage = v.obj("collage")

    private fun JsonElement.d() = jsonPrimitive.double
    private fun JsonElement.i() = jsonPrimitive.double.toInt()

    // --- Collage ------------------------------------------------------------------------------------------------------

    @Test
    fun theLayoutsAreTheWebsLayoutsCellForCell() {
        assertEquals(collage.getValue("max").i(), StoryCollage.MAX_PHOTOS)
        val web = collage.arr("layouts")
        assertEquals(web.size, StoryCollage.LAYOUTS.size)
        web.forEachIndexed { i, l ->
            val mine = StoryCollage.LAYOUTS[i]
            val o = l.jsonObject
            assertEquals(o.str("id"), mine.id)
            assertEquals(o.str("label"), mine.label)
            assertEquals(o.arr("cells").size, mine.cells.size)
            o.arr("cells").forEachIndexed { j, c ->
                val cell = c.jsonObject
                assertEquals("${mine.id} cell $j", listOf(cell.getValue("x").d(), cell.getValue("y").d(), cell.getValue("w").d(), cell.getValue("h").d()), listOf(mine.cells[j].x, mine.cells[j].y, mine.cells[j].w, mine.cells[j].h))
            }
        }
    }

    @Test
    fun theLayoutsOfferedForEachCountAreTheWebs() {
        for (e in collage.arr("layoutsFor")) {
            val o = e.jsonObject
            assertEquals("count ${o.getValue("count").i()}", o.arr("ids").map { it.jsonPrimitive.content }, StoryCollage.layoutsFor(o.getValue("count").i()).map { it.id })
        }
        assertTrue("no layout takes one photo or five", StoryCollage.layoutsFor(1).isEmpty() && StoryCollage.layoutsFor(5).isEmpty())
    }

    @Test
    fun cellSizesAndHowAPhotoFillsACellAreTheWebs() {
        val rects = collage.arr("cellRects")
        assertEquals(150, rects.size)
        for (e in rects) {
            val o = e.jsonObject
            val layout = StoryCollage.LAYOUTS.first { it.id == o.str("layout") }
            val mine = StoryCollage.cellRects(layout, o.getValue("w").d(), o.getValue("h").d(), o.getValue("gap").d())
            val web = o.arr("result")
            assertEquals(web.size, mine.size)
            web.forEachIndexed { i, r ->
                val c = r.jsonObject
                val at = "${layout.id} ${o.getValue("w").d()}x${o.getValue("h").d()} gap ${o.getValue("gap").d()} cell $i"
                assertEquals(at, listOf(c.getValue("x").d(), c.getValue("y").d(), c.getValue("w").d(), c.getValue("h").d()), listOf(mine[i].x, mine[i].y, mine[i].w, mine[i].h))
            }
        }
        for (e in collage.arr("fitCover")) {
            val o = e.jsonObject
            val mine = StoryCollage.fitCover(o.getValue("imageW").d(), o.getValue("imageH").d(), o.getValue("cellW").d(), o.getValue("cellH").d())
            val r = o.obj("result")
            assertEquals(o.toString(), listOf(r.getValue("sx").d(), r.getValue("sy").d(), r.getValue("sw").d(), r.getValue("sh").d()), listOf(mine.sx, mine.sy, mine.sw, mine.sh))
        }
    }

    // --- Templates ----------------------------------------------------------------------------------------------------

    @Test
    fun everyTemplateStartsAStoryTheWayTheWebsDoes() {
        val web = v.arr("templates")
        assertEquals(web.map { it.jsonObject.str("id") }, TextTemplates.ALL.map { it.id })
        val madeAt = v.getValue("templatesMadeAt").jsonPrimitive.content.toLong()
        for (e in web) {
            val o = e.jsonObject
            val template = TextTemplates.ALL.first { it.id == o.str("id") }
            assertEquals(o.str("label"), template.label)
            val applied = TextTemplates.apply(template, { "fixedid00000" }, madeAt)
            assertEquals(o.str("text"), applied.text)
            assertEquals(o.str("background"), applied.background.wire)
            assertEquals(o.str("font"), applied.font.wire)
            // The stickers are read back by the same parser a viewer uses, from what the web made (its ids fixed): equal, field for field.
            assertEquals(template.id, StoryStickerParser.parse(o.arr("stickers")), applied.stickers)
            assertEquals(o.arr("overlays").size, applied.overlays.size)
            o.arr("overlays").forEachIndexed { i, w ->
                val ov = w.jsonObject
                val mine = applied.overlays[i]
                assertEquals(ov.str("type") == "emoji", mine.isEmoji)
                assertEquals(ov.str("text"), mine.text)
                assertEquals(listOf(ov.getValue("x").d(), ov.getValue("y").d(), ov.getValue("scale").d(), ov.getValue("rotation").d()), listOf(mine.x, mine.y, mine.scale, mine.rotation))
                assertEquals(ov.str("color"), mine.color.wire)
                assertEquals(ov.str("font"), mine.font.wire)
                assertEquals(ov.getValue("boxed").toString().toBoolean(), mine.boxed)
                assertNull(mine.anim)
            }
        }
    }

    @Test
    fun theTemplatesOfferedAreAllButThePlainOneAndTheirStickersGetFreshIds() {
        assertEquals(TextTemplates.ALL.size - 1, TextTemplates.OFFERED.size)
        assertFalse(TextTemplates.OFFERED.any { it.id == "plain" })
        var counter = 0
        val withSticker = TextTemplates.ALL.first { it.id == "this-or-that" }
        val a = TextTemplates.apply(withSticker, { "id%09d".format(counter++) }, 0L)
        val b = TextTemplates.apply(withSticker, { "id%09d".format(counter++) }, 0L)
        assertEquals(1, a.stickers.size)
        assertTrue("a new id each time", a.stickers[0].id != b.stickers[0].id)
    }

    @Test
    fun aCountdownTemplateEndsAWeekAfterItWasUsed() {
        val countdown = TextTemplates.ALL.first { it.id == "countdown" }
        val applied = TextTemplates.apply(countdown, { "fixedid00000" }, 1_000L)
        assertEquals(1_000L + 7 * 86_400_000L, (applied.stickers.single() as com.redvaultsystems.redvaultmessage.data.model.CountdownSticker).endsAt)
    }

    // --- The composer using them ---------------------------------------------------------------------------------------

    @Test
    fun usingATemplateReplacesTheWordsLookStickersAndEmojiAndLetsGoOfTheSelection() {
        val model = ComposerModel(true)
        model.change { it.copy(text = "old", overlays = listOf(StoryOverlay(false, "Hi", 0.5, 0.5, 1.0, 0.0, StoryColor.White, false, StoryFont.Classic, null))) }
        model.selected = Selected.Overlay(0)
        model.useTemplate(TextTemplates.ALL.first { it.id == "countdown" }, { "fixedid00000" }, 1_000L)
        val s = model.slide
        assertEquals(SlideMode.Text, s.mode)
        assertEquals("Coming soon", s.text)
        assertEquals("night", s.background.wire)
        assertEquals("strong", s.font.wire)
        assertEquals(listOf("🎉"), s.overlays.map { it.text })
        assertEquals(1, s.stickers.size)
        assertNull(model.selected)
        assertTrue(model.dirty)
    }

    @Test
    fun theMotionButtonGoesStillPulseBounceFadeAndRoundAgainAndTouchesOnlyThatOverlay() {
        val model = ComposerModel(true)
        fun overlay(text: String) = StoryOverlay(false, text, 0.5, 0.5, 1.0, 0.0, StoryColor.White, false, StoryFont.Classic, null)
        model.change { it.copy(overlays = listOf(overlay("a"), overlay("b"))) }
        val seen = ArrayList<StoryAnimation?>()
        repeat(5) { model.cycleMotion(1); seen += model.slide.overlays[1].anim }
        assertEquals(listOf(StoryAnimation.Pulse, StoryAnimation.Bounce, StoryAnimation.Fade, null, StoryAnimation.Pulse), seen)
        assertNull("the other one is untouched", model.slide.overlays[0].anim)
        model.cycleMotion(7)
        assertEquals(listOf("Still", "Pulse", "Bounce", "Fade"), listOf(null, StoryAnimation.Pulse, StoryAnimation.Bounce, StoryAnimation.Fade).map { StoryMotion.label(it) })
    }

    // --- Motion ---------------------------------------------------------------------------------------------------------

    private fun near(expected: Double, actual: Float, delta: Double = 0.002) = assertEquals(expected, actual.toDouble(), delta)

    @Test
    fun theEasingIsCssEaseInOut() {
        assertEquals(0.0, StoryMotion.ease(0.0), 0.0)
        assertEquals(1.0, StoryMotion.ease(1.0), 0.0)
        assertEquals(0.5, StoryMotion.ease(0.5), 1e-6)
        // Symmetric, slow at the start and end, quick in the middle (cubic-bezier(0.42, 0, 0.58, 1)).
        for (x in listOf(0.1, 0.25, 0.4)) assertEquals(1.0, StoryMotion.ease(x) + StoryMotion.ease(1 - x), 1e-6)
        assertTrue(StoryMotion.ease(0.25) < 0.25)
        assertTrue(StoryMotion.ease(0.75) > 0.75)
        // A known point of the curve: y at x = 0.25 is 0.1291 (to four places).
        assertEquals(0.1291, StoryMotion.ease(0.25), 0.0005)
    }

    @Test
    fun aPulseGrowsToOnePointOneTwoAtHalfwayAndBackEveryOnePointFourSeconds() {
        near(1.0, StoryMotion.frame(StoryAnimation.Pulse, 0).scale)
        near(1.12, StoryMotion.frame(StoryAnimation.Pulse, 700).scale, 1e-6)
        near(1.06, StoryMotion.frame(StoryAnimation.Pulse, 350).scale, 1e-4)
        near(1.06, StoryMotion.frame(StoryAnimation.Pulse, 1050).scale, 1e-4)
        near(1.0, StoryMotion.frame(StoryAnimation.Pulse, 1400).scale)
        near(1.12, StoryMotion.frame(StoryAnimation.Pulse, 1400 * 5 + 700).scale, 1e-6)
        val f = StoryMotion.frame(StoryAnimation.Pulse, 700)
        assertEquals(0f, f.liftOfHeight, 0f)
        assertEquals(1f, f.alpha, 0f)
    }

    @Test
    fun aBounceLiftsFourteenPercentByFortyPercentOfASecondAndRestsFromSixty() {
        near(0.0, StoryMotion.frame(StoryAnimation.Bounce, 0).liftOfHeight)
        near(-0.14, StoryMotion.frame(StoryAnimation.Bounce, 400).liftOfHeight, 1e-6)
        near(-0.07, StoryMotion.frame(StoryAnimation.Bounce, 200).liftOfHeight, 1e-4)
        near(-0.07, StoryMotion.frame(StoryAnimation.Bounce, 500).liftOfHeight, 1e-4)
        near(0.0, StoryMotion.frame(StoryAnimation.Bounce, 600).liftOfHeight)
        near(0.0, StoryMotion.frame(StoryAnimation.Bounce, 800).liftOfHeight)
        near(-0.14, StoryMotion.frame(StoryAnimation.Bounce, 3400).liftOfHeight, 1e-6)
        assertEquals(1f, StoryMotion.frame(StoryAnimation.Bounce, 400).scale, 0f)
    }

    @Test
    fun aFadeDimsToThirtyFivePercentAtHalfwayOverTwoPointTwoSeconds() {
        near(1.0, StoryMotion.frame(StoryAnimation.Fade, 0).alpha)
        near(0.35, StoryMotion.frame(StoryAnimation.Fade, 1100).alpha, 1e-6)
        near(0.675, StoryMotion.frame(StoryAnimation.Fade, 550).alpha, 1e-4)
        near(1.0, StoryMotion.frame(StoryAnimation.Fade, 2200).alpha)
        assertEquals(1f, StoryMotion.frame(StoryAnimation.Fade, 1100).scale, 0f)
    }

    @Test
    fun noMotionIsAlwaysTheRestingFrameAndTimeBeforeTheStartIsHandled() {
        assertEquals(MotionFrame.STILL, StoryMotion.frame(null, 12_345))
        near(1.12, StoryMotion.frame(StoryAnimation.Pulse, -700).scale, 1e-6)
    }

}
