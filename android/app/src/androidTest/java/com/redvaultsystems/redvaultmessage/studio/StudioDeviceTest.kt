// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.studio

import android.Manifest
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.net.Uri
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.rule.GrantPermissionRule
import com.redvaultsystems.redvaultmessage.data.StoryInputException
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import kotlin.math.abs

/**
 * The studio's Android parts, on a device: decoding the audio files a phone can open, the recorder, drawing a photo edit with Android's bitmaps,
 * and the notification channel for story notices. (The maths itself is checked against the web's on the JVM: StudioVectorsTest.)
 * The sound files are in src/test/resources/vectors/audio (this test's assets).
 */
@RunWith(AndroidJUnit4::class)
class StudioDeviceTest {
    @get:Rule val permissions: GrantPermissionRule = GrantPermissionRule.grant(Manifest.permission.RECORD_AUDIO, Manifest.permission.POST_NOTIFICATIONS)

    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context = instrumentation.targetContext

    private fun asset(name: String): Uri {
        val out = File(context.cacheDir, "studio-test-$name")
        instrumentation.context.assets.open("audio/$name").use { input -> out.outputStream().use { input.copyTo(it) } }
        return Uri.fromFile(out)
    }

    // --- Audio files ----------------------------------------------------------------------------------------------

    @Test
    fun theFileTypesAPhoneOpensAreDecodedToMonoAtTheRateAStoryKeeps() = runBlocking {
        for (name in listOf("tone.m4a", "tone.mp3", "tone_stereo_44100.wav")) {
            val mono = AudioFileDecoder.decode(context, asset(name))
            assertTrue("$name rate ${mono.sampleRate}", mono.sampleRate <= AudioDsp.STORY_AUDIO_RATE)
            val ms = AudioDsp.durationMs(mono)
            // An encoder pads its ends a little; the tone is 0.8 s.
            assertTrue("$name is ${ms} ms long", ms in 700..1000)
            // The tone is made at an eighth of full scale (0.125).
            assertTrue("$name has sound in it (peak ${AudioDsp.peak(mono.samples)})", AudioDsp.peak(mono.samples) > 0.05)
        }
    }

    @Test
    fun aTooShortFileAndAFileThatIsNotAudioAreRefusedInPlainWords() = runBlocking {
        val short = runCatching { AudioFileDecoder.decode(context, asset("too_short.mp3")) }.exceptionOrNull()
        assertTrue("short: $short", short is StoryInputException)
        assertEquals("That recording is too short.", short!!.message)
        val text = runCatching { AudioFileDecoder.decode(context, asset("not_audio.txt")) }.exceptionOrNull()
        assertTrue("text: $text", text is StoryInputException)
    }

    @Test
    fun aVoiceTheRecorderMakesCanBeCutFilteredAndWrittenAsAWav() {
        val recorder = StoryVoiceRecorder()
        assertTrue("the microphone opens", recorder.start(5_000) {})
        // An emulator's virtual microphone can be slow to start delivering sound, so wait for half a second of it (at most five seconds) rather than sleeping a fixed time.
        var waited = 0
        while (recorder.elapsedMs < 500 && waited < 5_000) {
            Thread.sleep(50)
            waited += 50
        }
        val mono = recorder.stop()
        assertNotNull(mono)
        val ms = AudioDsp.durationMs(mono!!)
        assertTrue("recorded $ms ms", ms in 400..5_000)
        assertNull("a second stop has nothing", recorder.stop())
        val done = AudioDsp.finishVoice(mono, 0.0, ms.toDouble(), VoiceFilter.Radio)
        assertTrue(done.wav.size > 44)
        assertEquals(48, done.waveform.size)
        assertNotNull(AudioDsp.decodeWav(done.wav))
    }

    // --- Photo editing with Android's bitmaps -----------------------------------------------------------------------

    /** 40 by 20: red top left, green top right, blue bottom left, yellow bottom right. */
    private fun quadrants(): Bitmap {
        val b = Bitmap.createBitmap(40, 20, Bitmap.Config.ARGB_8888)
        for (y in 0 until 20) for (x in 0 until 40) {
            b.setPixel(x, y, when {
                y < 10 && x < 20 -> Color.RED
                y < 10 -> Color.GREEN
                x < 20 -> Color.BLUE
                else -> Color.YELLOW
            })
        }
        return b
    }

    private fun close(expected: Int, actual: Int, by: Int = 12): Boolean =
        abs(Color.red(expected) - Color.red(actual)) <= by && abs(Color.green(expected) - Color.green(actual)) <= by && abs(Color.blue(expected) - Color.blue(actual)) <= by

    @Test
    fun aQuarterTurnMovesTheCornersTheWayTheWebDoesAndAFlipMirrorsTheFinishedPicture() {
        val src = quadrants()
        val turned = ImageRender.draw(src, EditState(rotation = Rotation.R90), 1000)
        assertEquals(20 to 40, turned.width to turned.height)
        // Turned clockwise: the top left (red) goes to the top right; the bottom left (blue) to the top left.
        assertTrue(close(Color.BLUE, turned.getPixel(2, 2)))
        assertTrue(close(Color.RED, turned.getPixel(17, 2)))
        assertTrue(close(Color.YELLOW, turned.getPixel(2, 37)))
        assertTrue(close(Color.GREEN, turned.getPixel(17, 37)))
        val flipped = ImageRender.draw(src, EditState(flipX = true), 1000)
        assertTrue(close(Color.GREEN, flipped.getPixel(2, 2)))
        assertTrue(close(Color.RED, flipped.getPixel(37, 2)))
    }

    @Test
    fun aCropKeepsOnlyItsPartAndTheOutputIsScaledDownToTheLimit() {
        val src = quadrants()
        val state = EditState(crop = Rect(0.5, 0.5, 0.5, 0.5))
        val out = ImageRender.draw(src, state, 1000)
        assertEquals(20 to 10, out.width to out.height)
        assertTrue(close(Color.YELLOW, out.getPixel(1, 1)))
        assertTrue(close(Color.YELLOW, out.getPixel(18, 8)))
        val small = ImageRender.draw(src, EditState.NEUTRAL, 20)
        assertEquals(20 to 10, small.width to small.height)
    }

    @Test
    fun colourChangesDrawingAndTheEraserActLikeTheWebsAndTheEraserNeverTouchesThePhoto() {
        val src = quadrants()
        val mono = ImageRender.draw(src, EditState(filter = PhotoFilter.Mono), 1000)
        val grey = mono.getPixel(2, 2)
        assertTrue("mono makes red grey (${Color.red(grey)}, ${Color.green(grey)}, ${Color.blue(grey)})", abs(Color.red(grey) - Color.green(grey)) < 8 && abs(Color.green(grey) - Color.blue(grey)) < 8)
        val line = Stroke(StoryColor.White, 0.2, false, listOf(Point(0.0, 0.5), Point(1.0, 0.5)))
        val drawn = ImageRender.draw(src, EditState(strokes = listOf(line)), 1000)
        assertTrue("the line is white across the middle", close(Color.WHITE, drawn.getPixel(20, 10), 20))
        assertTrue("the top left corner is untouched", close(Color.RED, drawn.getPixel(2, 2)))
        val erase = Stroke(StoryColor.White, 0.2, true, listOf(Point(0.0, 0.5), Point(1.0, 0.5)))
        val erased = ImageRender.draw(src, EditState(strokes = listOf(line, erase)), 1000)
        // The eraser takes the drawing away and shows the photo beneath, never a hole in it.
        assertTrue("the photo shows again (${Integer.toHexString(erased.getPixel(30, 11))})", close(Color.YELLOW, erased.getPixel(30, 11)) || close(Color.GREEN, erased.getPixel(30, 9)))
        assertEquals(255, Color.alpha(erased.getPixel(30, 11)))
    }

    @Test
    fun aBlurSoftensAnEdgeAndAJpegComesOutWithTheRightSizeAndNoMetadata() {
        // The blur is up to 1% of the picture's height, so it needs a picture big enough to show it: 400 by 200, red left, green right.
        val big = Bitmap.createBitmap(400, 200, Bitmap.Config.ARGB_8888)
        for (y in 0 until 200) for (x in 0 until 400) big.setPixel(x, y, if (x < 200) Color.RED else Color.GREEN)
        val sharp = ImageRender.draw(big, EditState.NEUTRAL, 1000)
        val soft = ImageRender.draw(big, EditState(adjust = Adjust(blur = 100.0)), 1000)
        assertTrue("the sharp edge jumps from red to green", Color.green(sharp.getPixel(198, 100)) < 10 && Color.green(sharp.getPixel(201, 100)) > 240)
        val near = (196..203).map { Color.green(soft.getPixel(it, 100)) }
        assertTrue("the blurred edge has steps in between: $near", near.any { it in 30..225 })
        assertTrue("far from the edge nothing changes", close(Color.RED, soft.getPixel(20, 100), 4) && close(Color.GREEN, soft.getPixel(380, 100), 4))
        val (bytes, w, h) = ImageRender.renderJpeg(quadrants(), EditState(rotation = Rotation.R90), 1000)
        assertEquals(20 to 40, w to h)
        assertTrue("a JPEG starts with FFD8", bytes[0] == 0xFF.toByte() && bytes[1] == 0xD8.toByte())
        val back = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        assertEquals(20 to 40, back.width to back.height)
        // No Exif segment (APP1, "Exif") anywhere near the start: a bitmap is compressed from its pixels alone.
        assertTrue("no Exif", String(bytes.copyOf(200), Charsets.ISO_8859_1).indexOf("Exif") < 0)
    }

    // --- Collage with Android's bitmaps ---------------------------------------------------------------------------------

    private fun solid(color: Int, w: Int, h: Int): Bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888).apply { eraseColor(color) }

    @Test
    fun aGridOfFourPhotosFillsItsCellsAndLeavesTheGapInTheBackgroundColour() {
        val photos = listOf(Color.RED, Color.GREEN, Color.BLUE, Color.YELLOW).map { solid(it, 300, 500) }
        val background = 0xFF111111.toInt()
        val out = StoryCollage.draw(photos, StoryCollage.layoutsFor(4).single(), 400, 800, 20.0, background)
        assertEquals(400 to 800, out.width to out.height)
        // Each cell is 200 by 400, pulled in by 10 pixels all round.
        assertTrue("red top left", close(Color.RED, out.getPixel(100, 200)))
        assertTrue("green top right", close(Color.GREEN, out.getPixel(300, 200)))
        assertTrue("blue bottom left", close(Color.BLUE, out.getPixel(100, 600)))
        assertTrue("yellow bottom right", close(Color.YELLOW, out.getPixel(300, 600)))
        assertTrue("the gap between cells is the background", close(background, out.getPixel(200, 200), 3) && close(background, out.getPixel(100, 400), 3))
        assertTrue("and so is the edge", close(background, out.getPixel(4, 4), 3) && close(background, out.getPixel(395, 795), 3))
    }

    @Test
    fun aWidePhotoInATallCellShowsItsMiddleAndIsNeverStretched() {
        // 400 by 200: red on the left half, green on the right. Two side by side cells (200 by 800 each, no gap) show only the middle 50 pixels.
        val wide = Bitmap.createBitmap(400, 200, Bitmap.Config.ARGB_8888)
        for (y in 0 until 200) for (x in 0 until 400) wide.setPixel(x, y, if (x < 200) Color.RED else Color.GREEN)
        val out = StoryCollage.draw(listOf(wide, wide), StoryCollage.layoutsFor(2).first { it.id == "side2" }, 400, 800, 0.0, Color.BLACK)
        assertTrue("left of the middle is red (${Integer.toHexString(out.getPixel(40, 400))})", close(Color.RED, out.getPixel(40, 400)))
        assertTrue("right of the middle is green (${Integer.toHexString(out.getPixel(160, 400))})", close(Color.GREEN, out.getPixel(160, 400)))
        assertTrue("the same in the second cell", close(Color.RED, out.getPixel(240, 400)) && close(Color.GREEN, out.getPixel(360, 400)))
    }

    @Test
    fun aCollageIsAJpegOfStorySizeWithNoMetadataAndANumberOfPhotosThatDoesNotFitTheLayoutIsRefused() {
        val photos = listOf(Color.RED, Color.BLUE).map { solid(it, 600, 400) }
        val (bytes, w, h) = StoryCollage.renderJpeg(photos, StoryCollage.layoutsFor(2).first(), 12.0, StoryCollage.BACKGROUNDS[1].argb)
        assertEquals(1080 to 1920, w to h)
        assertTrue("a JPEG starts with FFD8", bytes[0] == 0xFF.toByte() && bytes[1] == 0xD8.toByte())
        assertTrue("no Exif", String(bytes.copyOf(200), Charsets.ISO_8859_1).indexOf("Exif") < 0)
        val back = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        assertEquals(1080 to 1920, back.width to back.height)
        // The "Light" background shows at the edge (JPEG blurs colour a little where it meets a photo, so allow for that).
        assertTrue("light edge (${Integer.toHexString(back.getPixel(2, 2))})", close(StoryCollage.BACKGROUNDS[1].argb, back.getPixel(2, 2), 24))
        val refused = runCatching { StoryCollage.draw(photos, StoryCollage.layoutsFor(3).first(), 100, 100) }.exceptionOrNull()
        assertTrue("three-photo layout, two photos: $refused", refused is IllegalArgumentException)
    }

}
