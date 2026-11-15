// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.ParcelFileDescriptor
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.redvaultsystems.redvaultmessage.data.PreparedStoryMedia
import com.redvaultsystems.redvaultmessage.data.model.StoryAnimation
import com.redvaultsystems.redvaultmessage.studio.StoryMotion
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * The collage screen used on a device (put photos in order, remove one, the layouts offered for what is left, Done making a story photo) and
 * animated words playing in the viewer (they move with time, stand still while the story is held, and stand still when the phone has
 * animations removed). The screens are driven by the Compose test clock, so time passes exactly as asked.
 */
@RunWith(AndroidJUnit4::class)
class CollageAndMotionUiTest {
    @get:Rule val rule = createComposeRule()

    private fun solid(color: Int, w: Int = 120, h: Int = 200) = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888).apply { eraseColor(color) }

    private fun photos(count: Int): List<Pair<String, Bitmap>> =
        listOf("A" to solid(0xFFCC2222.toInt()), "B" to solid(0xFF22CC22.toInt()), "C" to solid(0xFF2222CC.toInt()), "D" to solid(0xFFCCCC22.toInt())).take(count)

    private var made: PreparedStoryMedia? = null

    private fun show(count: Int) {
        made = null
        rule.setContent { RedvaultTheme { CollageScreen(onDone = { made = it }, onCancel = {}, startWith = photos(count)) } }
    }

    @Test
    fun photosCanBePutInOrderAndRemovedAndTheLayoutsFollowTheCount() {
        show(3)
        rule.onNodeWithText("1. A").assertExists()
        rule.onNodeWithText("Three, stacked").assertExists()
        rule.onNodeWithText("One big, two small").assertExists()
        rule.onNodeWithContentDescription("Move B earlier").performClick()
        rule.waitForIdle()
        rule.onNodeWithText("1. B").assertExists()
        rule.onNodeWithText("2. A").assertExists()
        // The first can't go earlier and the last can't go later.
        rule.onNodeWithContentDescription("Move B earlier").assertIsNotEnabled()
        rule.onNodeWithContentDescription("Move C later").assertIsNotEnabled()
        rule.onNodeWithContentDescription("Remove C").performClick()
        rule.waitForIdle()
        rule.onNodeWithText("Two, stacked").assertExists()
        rule.onNodeWithText("Two, side by side").assertExists()
        assertTrue("no three-photo layout is offered for two photos", rule.onAllNodes(hasText("Three, stacked")).fetchSemanticsNodes().isEmpty())
        rule.onNodeWithText("Add a photo").assertExists()
    }

    @Test
    fun doneIsNotOfferedForOnePhotoAndTheScreenSaysWhatToDo() {
        show(1)
        rule.onNodeWithText("Done").assertIsNotEnabled()
        rule.onNodeWithText("Choose two to four photos. They stay on this phone.").assertExists()
        rule.onNodeWithText("Add a photo").assertExists()
    }

    @Test
    fun doneFromTwoPhotosMakesAStoryPhotoOfTheRightSize() {
        show(2)
        rule.onNodeWithText("Done").assertIsEnabled()
        rule.onNodeWithText("Done").performClick()
        rule.waitUntil(10_000) { made != null }
        val m = made!!
        assertEquals("image/jpeg", m.mime)
        assertEquals(1080 to 1920, m.width to m.height)
        val bytes = m.open().use { it.readBytes() }
        assertEquals(bytes.size.toLong(), m.sizeBytes)
        assertTrue("a JPEG starts with FFD8", bytes[0] == 0xFF.toByte() && bytes[1] == 0xD8.toByte())
        assertTrue("no Exif", String(bytes.copyOf(200), Charsets.ISO_8859_1).indexOf("Exif") < 0)
        val back = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        assertEquals(1080 to 1920, back.width to back.height)
    }

    @Test
    fun withNoPhotosItAsksForThemAndOffersNothingToMakeYet() {
        show(0)
        rule.onNodeWithText("Choose two to four photos. They stay on this phone.").assertExists()
        rule.onNodeWithText("Choose photos").assertExists()
        rule.onNodeWithText("Done").assertIsNotEnabled()
        assertTrue(rule.onAllNodes(hasContentDescription("Collage preview")).fetchSemanticsNodes().isEmpty())
    }

    // --- Animated words -------------------------------------------------------------------------------------------

    private var paused by mutableStateOf(false)

    @Composable
    private fun Readout(tag: String, anim: StoryAnimation?) {
        val frame = rememberMotion(anim, paused)
        // The state is read here to show it; in the viewer it is read when drawing.
        Text("$tag scale=%.3f lift=%.3f alpha=%.3f".format(frame.value.scale, frame.value.liftOfHeight, frame.value.alpha))
    }

    private fun readout(tag: String): Triple<Double, Double, Double> {
        val text = rule.onNode(hasText("$tag scale=", substring = true)).fetchSemanticsNode().config.getOrNull(SemanticsProperties.Text)!!.first().text
        val n = Regex("scale=(-?[0-9.]+) lift=(-?[0-9.]+) alpha=(-?[0-9.]+)").find(text)!!.groupValues
        return Triple(n[1].toDouble(), n[2].toDouble(), n[3].toDouble())
    }

    private fun shell(command: String): String {
        val pfd = InstrumentationRegistry.getInstrumentation().uiAutomation.executeShellCommand(command)
        return ParcelFileDescriptor.AutoCloseInputStream(pfd).use { it.readBytes().toString(Charsets.UTF_8).trim() }
    }

    private fun withAnimationScale(scale: String, body: () -> Unit) {
        val before = shell("settings get global animator_duration_scale").let { if (it.isBlank() || it == "null") "1.0" else it }
        shell("settings put global animator_duration_scale $scale")
        try {
            body()
        } finally {
            shell("settings put global animator_duration_scale $before")
        }
    }

    @Test
    fun aPulsingWordGrowsWithTimeAndAFadingOneDimsAndABouncingOneLifts() {
        paused = false
        rule.mainClock.autoAdvance = false
        rule.setContent {
            RedvaultTheme {
                Column {
                    Readout("pulse", StoryAnimation.Pulse)
                    Readout("bounce", StoryAnimation.Bounce)
                    Readout("fade", StoryAnimation.Fade)
                    Readout("still", null)
                }
            }
        }
        rule.mainClock.advanceTimeByFrame()
        // Just after the start every word is at rest.
        for (tag in listOf("pulse", "bounce", "fade", "still")) {
            val start = readout(tag)
            assertEquals("$tag at rest: $start", Triple(1.0, 0.0, 1.0).first, start.first, 0.01)
            assertEquals("$tag at rest: $start", 0.0, start.second, 0.01)
            assertEquals("$tag at rest: $start", 1.0, start.third, 0.01)
        }
        // Run to 400 ms: the bounce is at its top.
        rule.mainClock.advanceTimeBy(400)
        assertTrue("bounce at 400 ms: ${readout("bounce")}", readout("bounce").second < -0.12)
        // Then to 700 ms: the pulse is at its biggest.
        rule.mainClock.advanceTimeBy(300)
        assertTrue("pulse at 700 ms: ${readout("pulse")}", readout("pulse").first > 1.10)
        // Then to 1100 ms: the fade is at its dimmest.
        rule.mainClock.advanceTimeBy(400)
        assertTrue("fade at 1100 ms: ${readout("fade")}", readout("fade").third < 0.40)
        assertEquals("a word with no motion never moves", Triple(1.0, 0.0, 1.0), readout("still"))
        rule.mainClock.autoAdvance = true
    }

    @Test
    fun aHeldStoryHoldsItsWordsStillAndTheyCarryOnFromThere() {
        rule.mainClock.autoAdvance = false
        paused = false
        rule.setContent { RedvaultTheme { Readout("w", StoryAnimation.Pulse) } }
        rule.mainClock.advanceTimeBy(300)
        val moving = readout("w")
        paused = true
        rule.mainClock.advanceTimeByFrame()
        rule.mainClock.advanceTimeBy(100)
        val held = readout("w")
        rule.mainClock.advanceTimeBy(500)
        assertEquals("held for a while, still the same", held, readout("w"))
        paused = false
        rule.mainClock.advanceTimeByFrame()
        rule.mainClock.advanceTimeBy(200)
        val after = readout("w")
        assertTrue("it moved again: $moving $held $after", after.first != held.first)
        rule.mainClock.autoAdvance = true
    }

    @Test
    fun whenThePhoneHasAnimationsRemovedWordsStayStill() {
        withAnimationScale("0") {
            assertTrue(StoryMotion.reduced(InstrumentationRegistry.getInstrumentation().targetContext))
            rule.mainClock.autoAdvance = false
            paused = false
            rule.setContent { RedvaultTheme { Readout("w", StoryAnimation.Pulse) } }
            rule.mainClock.advanceTimeBy(700)
            assertEquals(Triple(1.0, 0.0, 1.0), readout("w"))
            rule.mainClock.autoAdvance = true
        }
        assertTrue("and moving again once they are back", !StoryMotion.reduced(InstrumentationRegistry.getInstrumentation().targetContext))
    }
}
