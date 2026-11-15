// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.stories

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.data.model.StoryCaption
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.studio.Adjust
import com.redvaultsystems.redvaultmessage.studio.AudioDsp
import com.redvaultsystems.redvaultmessage.studio.ColorParams
import com.redvaultsystems.redvaultmessage.studio.EditState
import com.redvaultsystems.redvaultmessage.studio.Handle
import com.redvaultsystems.redvaultmessage.studio.ImageEdit
import com.redvaultsystems.redvaultmessage.studio.Mono
import com.redvaultsystems.redvaultmessage.studio.PhotoFilter
import com.redvaultsystems.redvaultmessage.studio.Point
import com.redvaultsystems.redvaultmessage.studio.Rect
import com.redvaultsystems.redvaultmessage.studio.Rotation
import com.redvaultsystems.redvaultmessage.studio.Stroke
import com.redvaultsystems.redvaultmessage.studio.StoryCaptions
import com.redvaultsystems.redvaultmessage.studio.VoiceFilter
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.double
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The studio's maths (subtitles, voice, photo editing) against what the web app's own code makes of the same inputs
 * (resources/vectors/studio_vectors.json, made by make_android_studio_vectors.ts). Sound is compared sample by sample to 2e-5
 * (JavaScript and the JVM can differ in the last bit of a sine); everything else is exact.
 */
class StudioVectorsTest {
    private val v = vectors("studio_vectors.json")
    private val captions = v.obj("captions")
    private val audio = v.obj("audio")
    private val image = v.obj("image")

    private fun JsonElement.d() = jsonPrimitive.double
    private fun JsonElement.i() = jsonPrimitive.double.toInt()
    private fun JsonElement.l() = jsonPrimitive.double.toLong()
    private fun JsonElement.s() = jsonPrimitive.content
    private fun JsonElement.b() = jsonPrimitive.boolean
    private fun JsonElement.isNull() = this is JsonNull

    private fun floats(base64: String): FloatArray {
        val bytes = Base64Codec.decode(base64)
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        return FloatArray(bytes.size / 4) { buffer.getFloat(it * 4) }
    }

    private fun mono(e: JsonElement) = Mono(floats(e.jsonObject.str("samples")), e.jsonObject.getValue("rate").d())

    private fun assertMono(at: String, expected: JsonElement, actual: Mono, tolerance: Double = 2e-5) {
        val want = mono(expected)
        assertEquals("$at: rate", want.sampleRate, actual.sampleRate, 0.0)
        assertEquals("$at: length", want.samples.size, actual.samples.size)
        for (i in want.samples.indices) assertEquals("$at: sample $i", want.samples[i].toDouble(), actual.samples[i].toDouble(), tolerance)
    }

    private val inputs: Map<String, Mono> = audio.obj("inputs").mapValues { mono(it.value) }
    private fun input(name: String) = inputs.getValue(name)

    // --- Subtitles ----------------------------------------------------------------------------------------------

    @Test
    fun clockTextsAreReadAsTheWebReadsThem() {
        val cases = captions.arr("parseClock")
        assertTrue(cases.size > 20)
        for (c in cases.map { it.jsonObject }) {
            val want = c.getValue("ms").let { if (it.isNull()) null else it.l() }
            assertEquals("parseClock(\"${c.str("text")}\")", want, StoryCaptions.parseClock(c.str("text")))
        }
    }

    @Test
    fun clockTimesAreWrittenAsTheWebWritesThem() {
        val cases = captions.arr("formatClock")
        assertTrue(cases.size > 600)
        for (c in cases.map { it.jsonObject }) assertEquals("formatClock(${c.getValue("ms").l()})", c.str("text"), StoryCaptions.formatClock(c.getValue("ms").l()))
        // The web's own rounding quirk, kept: 59.95 seconds is written as a minute that has sixty seconds.
        assertEquals("0:60.0", StoryCaptions.formatClock(59_950L))
        assertEquals("0:59.9", StoryCaptions.formatClock(59_949L))
        assertEquals("0:00.0", StoryCaptions.formatClock(-5.0))
    }

    private fun assertCaptions(at: String, expected: JsonElement, actual: List<StoryCaption>) {
        val want = expected.jsonArray.map { it.jsonObject }
        assertEquals("$at: how many", want.size, actual.size)
        want.zip(actual).forEachIndexed { i, (w, a) ->
            assertEquals("$at: line $i start", w.getValue("startMs").l(), a.startMs)
            assertEquals("$at: line $i end", w.getValue("endMs").l(), a.endMs)
            assertEquals("$at: line $i text", w.str("text"), a.text)
        }
    }

    @Test
    fun srtFilesAreReadAsTheWebReadsThem() {
        for ((n, c) in captions.arr("parseSrt").map { it.jsonObject }.withIndex()) {
            assertCaptions("srt $n", c.getValue("captions"), StoryCaptions.parseSrt(c.str("srt")))
        }
    }

    @Test
    fun captionsAreWrittenAsSrtAsTheWebWritesThem() {
        val c = captions.obj("toSrt")
        val list = c.arr("captions").map { it.jsonObject }.map { StoryCaption(it.getValue("startMs").l(), it.getValue("endMs").l(), it.str("text")) }
        assertEquals(c.str("srt"), StoryCaptions.toSrt(list))
        // What is written can be read back.
        assertEquals(list.map { Triple(it.startMs, it.endMs, it.text) }, StoryCaptions.parseSrt(StoryCaptions.toSrt(list)).map { Triple(it.startMs, it.endMs, it.text) })
    }

    @Test
    fun typedLinesAreSpreadEvenlyAsTheWebSpreadsThem() {
        for ((n, c) in captions.arr("evenCaptions").map { it.jsonObject }.withIndex()) {
            val lines = c.arr("lines").map { it.s() }
            assertCaptions("even $n", c.getValue("captions"), StoryCaptions.evenCaptions(lines, c.getValue("durationMs").l()))
        }
    }

    // --- Voice ------------------------------------------------------------------------------------------------------------

    @Test
    fun resamplingTrimmingFadingEvenOutAndGainMatchTheWeb() {
        for (c in audio.arr("resample").map { it.jsonObject }) assertMono("resample ${c.str("input")} to ${c.getValue("rate").d()}", c.getValue("result"), AudioDsp.resample(input(c.str("input")), c.getValue("rate").d()))
        for (c in audio.arr("trim").map { it.jsonObject }) assertMono("trim ${c.str("input")} ${c.getValue("startMs").d()}..${c.getValue("endMs").d()}", c.getValue("result"), AudioDsp.trim(input(c.str("input")), c.getValue("startMs").d(), c.getValue("endMs").d()))
        for (c in audio.arr("fade").map { it.jsonObject }) assertMono("fade ${c.str("input")}", c.getValue("result"), AudioDsp.fade(input(c.str("input")), c.getValue("inMs").d(), c.getValue("outMs").d()))
        for (c in audio.arr("normalize").map { it.jsonObject }) {
            val target = c.getValue("target")
            val actual = if (target.isNull()) AudioDsp.normalize(input(c.str("input"))) else AudioDsp.normalize(input(c.str("input")), target.d())
            assertMono("normalize ${c.str("input")}", c.getValue("result"), actual)
        }
        for (c in audio.arr("gain").map { it.jsonObject }) assertMono("gain ${c.str("input")} ${c.getValue("factor").d()}", c.getValue("result"), AudioDsp.gain(input(c.str("input")), c.getValue("factor").d()))
        for (c in audio.arr("duration").map { it.jsonObject }) assertEquals(c.str("input"), c.getValue("ms").l(), AudioDsp.durationMs(input(c.str("input"))))
        val mix = audio.obj("mixToMono")
        assertMono("mixToMono", mix.getValue("result"), AudioDsp.mixToMono(mix.arr("channels").map { floats(it.s()) }, mix.getValue("rate").d()))
    }

    @Test
    fun everyVoiceFilterSoundsTheWayTheWebMakesIt() {
        val filters = audio.arr("filters").map { it.jsonObject }
        assertTrue(filters.size >= 8)
        for (c in filters) {
            val filter = VoiceFilter.entries.single { it.wire == c.str("filter") }
            assertMono("${c.str("filter")} on ${c.str("input")}", c.getValue("result"), AudioDsp.applyVoiceFilter(input(c.str("input")), filter))
        }
        // The six the web offers, in the order it offers them.
        assertEquals(listOf("none", "chipmunk", "deep", "robot", "echo", "radio"), VoiceFilter.entries.map { it.wire })
    }

    @Test
    fun theLoudnessBarsAreTheWebs() {
        for (c in audio.arr("waveform").map { it.jsonObject }) {
            assertEquals("bars of ${c.str("input")} x ${c.getValue("bars").i()}", c.arr("result").map { it.i() }, AudioDsp.waveformBars(input(c.str("input")).samples, c.getValue("bars").i()))
        }
    }

    @Test
    fun aWavIsWrittenByteForByteAsTheWebWritesItAndReadBack() {
        val wav = audio.obj("wav")
        val bytes = Base64Codec.decode(wav.str("bytes"))
        assertArrayEquals(bytes, AudioDsp.encodeWav(mono(wav.getValue("source"))))
        assertMono("decoded wav", wav.getValue("decoded"), AudioDsp.decodeWav(bytes)!!, 1e-6)
        val flags = audio.arr("notAWav").map { it.b() }
        assertEquals(listOf(true, true), flags)
        assertNull(AudioDsp.decodeWav(ByteArray(10)))
        assertNull(AudioDsp.decodeWav(bytes.copyOf(43)))
        // Only 16-bit mono PCM: a stereo or 8-bit header is refused.
        val stereo = bytes.copyOf().also { it[22] = 2 }
        assertNull(AudioDsp.decodeWav(stereo))
        val eightBit = bytes.copyOf().also { it[34] = 8 }
        assertNull(AudioDsp.decodeWav(eightBit))
        // A size that claims more than there is reads only what is there.
        val long = bytes.copyOf().also { ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN).putInt(40, 1_000_000) }
        assertEquals((bytes.size - 44) / 2, AudioDsp.decodeWav(long)!!.samples.size)
    }

    @Test
    fun aFinishedVoiceIsTheWebsFinishedVoice() {
        for (c in audio.arr("finish").map { it.jsonObject }) {
            val filter = VoiceFilter.entries.single { it.wire == c.str("filter") }
            val at = "finish ${c.str("input")} ${c.str("filter")}"
            val done = AudioDsp.finishVoice(input(c.str("input")), c.getValue("startMs").d(), c.getValue("endMs").d(), filter)
            assertEquals("$at: length of the sound", c.getValue("durationMs").l(), done.durationMs)
            assertEquals("$at: bars", c.arr("waveform").map { it.i() }, done.waveform)
            val want = Base64Codec.decode(c.str("wav"))
            assertEquals("$at: file size", want.size, done.wav.size)
            // The header is identical; each 16-bit sample may differ by a step or two from the last bit of a sine.
            assertArrayEquals("$at: header", want.copyOf(44), done.wav.copyOf(44))
            val a = ByteBuffer.wrap(want).order(ByteOrder.LITTLE_ENDIAN)
            val b = ByteBuffer.wrap(done.wav).order(ByteOrder.LITTLE_ENDIAN)
            for (k in 0 until (want.size - 44) / 2) assertEquals("$at: sample $k", a.getShort(44 + k * 2).toDouble(), b.getShort(44 + k * 2).toDouble(), 2.0)
        }
    }

    // --- Photos -----------------------------------------------------------------------------------------------------------

    private fun rect(e: JsonElement) = e.jsonObject.let { Rect(it.getValue("x").d(), it.getValue("y").d(), it.getValue("w").d(), it.getValue("h").d()) }
    private fun optRect(e: JsonElement) = if (e.isNull()) null else rect(e)
    private fun rotation(e: JsonElement) = Rotation.entries.single { it.degrees == e.i() }
    private fun point(e: JsonElement) = Point(e.jsonObject.getValue("x").d(), e.jsonObject.getValue("y").d())
    private fun stroke(e: JsonElement) = e.jsonObject.let { o ->
        Stroke(StoryColor.entries.single { it.wire == o.str("color") }, o.getValue("size").d(), o.getValue("eraser").b(), o.arr("points").map(::point))
    }
    private fun state(e: JsonElement): EditState = e.jsonObject.let { o ->
        val a = o.obj("adjust")
        EditState(
            rotation(o.getValue("rotation")), o.getValue("flipX").b(), optRect(o.getValue("crop")),
            Adjust(a.getValue("brightness").d(), a.getValue("contrast").d(), a.getValue("saturation").d(), a.getValue("blur").d()),
            PhotoFilter.entries.single { it.wire == o.str("filter") }, o.arr("strokes").map(::stroke),
        )
    }

    private fun assertRect(at: String, expected: JsonElement, actual: Rect?) {
        if (expected.isNull()) return assertNull(at, actual)
        val w = rect(expected)
        assertEquals("$at x", w.x, actual!!.x, 1e-12)
        assertEquals("$at y", w.y, actual.y, 1e-12)
        assertEquals("$at w", w.w, actual.w, 1e-12)
        assertEquals("$at h", w.h, actual.h, 1e-12)
    }

    @Test
    fun turningAndSizingMatchTheWeb() {
        for (c in image.arr("rotatedSize").map { it.jsonObject }) {
            val r = ImageEdit.rotatedSize(c.getValue("w").i(), c.getValue("h").i(), rotation(c.getValue("rotation")))
            assertEquals(c.getValue("result").jsonObject.getValue("w").i() to c.getValue("result").jsonObject.getValue("h").i(), r)
        }
        for (c in image.arr("rotateBy").map { it.jsonObject }) assertEquals("rotateBy ${c}", rotation(c.getValue("result")), ImageEdit.rotateBy(rotation(c.getValue("rotation")), c.getValue("quarterTurns").i()))
        for ((n, c) in image.arr("cropPixels").map { it.jsonObject }.withIndex()) {
            val p = ImageEdit.cropPixels(c.getValue("w").i(), c.getValue("h").i(), state(c.getValue("state")))
            val want = c.obj("result")
            assertEquals("cropPixels $n", listOf(want.getValue("sx").i(), want.getValue("sy").i(), want.getValue("sw").i(), want.getValue("sh").i()), listOf(p.sx, p.sy, p.sw, p.sh))
        }
        for ((n, c) in image.arr("outputSize").map { it.jsonObject }.withIndex()) {
            val s = ImageEdit.outputSize(c.getValue("w").i(), c.getValue("h").i(), state(c.getValue("state")), c.getValue("maxSide").i())
            assertEquals("outputSize $n", c.obj("result").getValue("w").i() to c.obj("result").getValue("h").i(), s)
        }
    }

    @Test
    fun cropsAreKeptInsideAndDraggedAsTheWebDragsThem() {
        for ((n, c) in image.arr("clampRect").map { it.jsonObject }.withIndex()) assertRect("clampRect $n", c.getValue("result"), ImageEdit.clampRect(rect(c.getValue("rect"))))
        for ((n, c) in image.arr("aspectCrop").map { it.jsonObject }.withIndex()) {
            val aspect = c.getValue("aspect").let { if (it.isNull()) null else it.d() }
            assertRect("aspectCrop $n", c.getValue("result"), ImageEdit.aspectCrop(aspect, c.getValue("w").i(), c.getValue("h").i()))
        }
        val drags = image.arr("dragRect").map { it.jsonObject }
        assertEquals(36, drags.size)
        for ((n, c) in drags.withIndex()) {
            val handle = Handle.entries.single { it.wire == c.str("handle") }
            assertRect("dragRect $n ${c.str("handle")}", c.getValue("result"), ImageEdit.dragRect(rect(c.getValue("rect")), handle, c.getValue("dx").d(), c.getValue("dy").d()))
        }
    }

    private fun assertParams(at: String, expected: JsonObject, actual: ColorParams) {
        assertEquals("$at brightness", expected.getValue("brightness").d(), actual.brightness, 1e-12)
        assertEquals("$at contrast", expected.getValue("contrast").d(), actual.contrast, 1e-12)
        assertEquals("$at saturate", expected.getValue("saturate").d(), actual.saturate, 1e-12)
        assertEquals("$at grayscale", expected.getValue("grayscale").d(), actual.grayscale, 1e-12)
        assertEquals("$at sepia", expected.getValue("sepia").d(), actual.sepia, 1e-12)
        assertEquals("$at hueRotate", expected.getValue("hueRotate").d(), actual.hueRotate, 1e-12)
        assertEquals("$at blurPx", expected.getValue("blurPx").d(), actual.blurPx, 1e-12)
    }

    @Test
    fun colourFiltersSlidersAndPixelsMatchTheWeb() {
        val params = image.arr("colorParams").map { it.jsonObject }
        assertTrue(params.size >= 10)
        for ((n, c) in params.withIndex()) assertParams("colorParams $n", c.obj("result"), ImageEdit.colorParams(state(c.getValue("state")), c.getValue("outputHeight").d()))
        val pixels = image.obj("pixels")
        val original = Base64Codec.decode(pixels.str("rgba"))
        for ((n, run) in pixels.arr("runs").map { it.jsonObject }.withIndex()) {
            val c = ImageEdit.colorParams(state(run.getValue("state")), 800.0)
            assertParams("pixel run $n params", run.obj("params"), c)
            val data = original.copyOf()
            ImageEdit.adjustPixels(data, c)
            assertArrayEquals("pixel run $n", Base64Codec.decode(run.str("result")), data)
        }
    }

    @Test
    fun hasEditsAndTheStrokeRulesMatchTheWeb() {
        for ((n, c) in image.arr("hasEdits").map { it.jsonObject }.withIndex()) assertEquals("hasEdits $n", c.getValue("result").b(), ImageEdit.hasEdits(state(c.getValue("state"))))
        var stroke = ImageEdit.newStroke(StoryColor.Blue, 0.2, false, Point(-1.0, 2.0))
        for ((n, step) in image.arr("strokeSteps").map { it.jsonObject }.withIndex()) {
            stroke = ImageEdit.extendStroke(stroke, point(step.getValue("point")))
            val want = stroke(step.getValue("stroke"))
            assertEquals("step $n size", want.size, stroke.size, 0.0)
            assertEquals("step $n points", want.points, stroke.points)
        }
        val long = image.obj("longStroke")
        var l = ImageEdit.newStroke(StoryColor.White, 0.001, true, Point(0.0, 0.0))
        for (i in 1 until 2100) l = ImageEdit.extendStroke(l, Point((i % 700) / 700.0, (i / 700) / 3.0))
        assertEquals(long.getValue("points").i(), l.points.size)
        assertEquals(point(long.getValue("first")), l.points.first())
        assertEquals(point(long.getValue("last")), l.points.last())
        val many = image.obj("manyStrokes")
        var s = EditState.NEUTRAL
        for (i in 0 until 410) s = ImageEdit.addStroke(s, ImageEdit.newStroke(StoryColor.Green, 0.01, i % 2 == 0, Point((i % 10) / 10.0, 0.5)))
        assertEquals(many.getValue("count").i(), s.strokes.size)
        assertEquals(many.getValue("firstEraser").b(), s.strokes.first().eraser)
        assertEquals(many.getValue("lastEraser").b(), s.strokes.last().eraser)
        for (c in image.arr("newStroke").map { it.jsonObject }) {
            val made = ImageEdit.newStroke(StoryColor.entries.single { it.wire == c.str("color") }, c.getValue("size").d(), c.getValue("eraser").b(), Point(0.3, 0.7))
            val want = stroke(c.getValue("result"))
            assertEquals(want.size, made.size, 0.0)
            assertEquals(want.points, made.points)
        }
        val g = image.obj("withGeometry")
        val after = ImageEdit.withGeometry(state(g.getValue("before")), rotation = Rotation.R180, flipX = true)
        assertEquals(state(g.getValue("result")), after)
        assertTrue(after.strokes.isEmpty())
        val consts = image.obj("constants")
        assertEquals(consts.getValue("minCrop").d(), ImageEdit.MIN_CROP, 0.0)
        assertEquals(consts.getValue("minPen").d(), ImageEdit.MIN_PEN, 0.0)
        assertEquals(consts.getValue("maxPen").d(), ImageEdit.MAX_PEN, 0.0)
        assertEquals(consts.getValue("maxStrokes").i(), ImageEdit.MAX_STROKES)
        assertEquals(consts.getValue("maxPoints").i(), ImageEdit.MAX_POINTS_PER_STROKE)
        assertEquals(consts.arr("filters").map { it.s() }, PhotoFilter.entries.map { it.wire })
        assertFalse(ImageEdit.hasEdits(EditState.NEUTRAL))
    }
}
