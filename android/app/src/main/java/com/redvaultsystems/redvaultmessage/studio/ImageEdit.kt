// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.studio

import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.data.model.jsRound
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

// Photo editing, all on this phone: crop, rotate, flip, colour correction, filters and drawing. The maths that decides what the result
// looks like is here, in plain functions pinned to ui/src/lib/studio/imageEdit.ts by studio_vectors.json (made by the web's own code);
// ImageRender.kt draws it with Android's bitmaps. An edit is a plain value ([EditState]), so undo is just keeping the old ones.
// Nothing is uploaded to edit a photo, and the result is a fresh JPEG that carries none of the original file's metadata.

/** A quarter turn at a time. */
enum class Rotation(val degrees: Int) {
    R0(0), R90(90), R180(180), R270(270),
}

/** A rectangle as fractions (0 to 1) of the image after rotation. */
data class Rect(val x: Double, val y: Double, val w: Double, val h: Double)

data class Adjust(
    /** -100 to 100; 0 changes nothing. */
    val brightness: Double = 0.0,
    val contrast: Double = 0.0,
    val saturation: Double = 0.0,
    /** 0 to 100 (up to about 1% of the image's height of blur). */
    val blur: Double = 0.0,
)

enum class PhotoFilter(val wire: String, val label: String) {
    None("none", "Original"), Vivid("vivid", "Vivid"), Warm("warm", "Warm"), Cool("cool", "Cool"),
    Fade("fade", "Fade"), Mono("mono", "Mono"), Noir("noir", "Noir"), Sepia("sepia", "Sepia"),
}

data class Point(val x: Double, val y: Double)

/** A line drawn on the result; points and size are fractions of the finished picture's width and height. */
data class Stroke(val color: StoryColor, val size: Double, val eraser: Boolean, val points: List<Point>)

data class EditState(
    val rotation: Rotation = Rotation.R0,
    val flipX: Boolean = false,
    /** Null: the whole picture. */
    val crop: Rect? = null,
    val adjust: Adjust = Adjust(),
    val filter: PhotoFilter = PhotoFilter.None,
    val strokes: List<Stroke> = emptyList(),
) {
    companion object {
        val NEUTRAL = EditState()
    }
}

/** Where a crop is dragged from. */
enum class Handle(val wire: String) {
    Move("move"), NW("nw"), NE("ne"), SW("sw"), SE("se"), N("n"), S("s"), E("e"), W("w"),
}

/** What a filter and the sliders do, as the terms the CSS filter functions use (1 = unchanged). */
data class ColorParams(
    val brightness: Double,
    val contrast: Double,
    val saturate: Double,
    val grayscale: Double,
    val sepia: Double,
    val hueRotate: Double,
    val blurPx: Double,
) {
    /** Whether anything about the colour changes at all. */
    val neutral: Boolean
        get() = near(brightness, 1.0) && near(contrast, 1.0) && near(saturate, 1.0) && grayscale <= 0 && sepia <= 0 && hueRotate == 0.0 && blurPx <= 0.05

    private fun near(v: Double, to: Double) = abs(v - to) < 1e-3
}

object ImageEdit {
    const val MIN_CROP = 0.08
    const val MIN_PEN = 0.004
    const val MAX_PEN = 0.08
    const val MAX_STROKES = 400
    const val MAX_POINTS_PER_STROKE = 2000

    fun clamp(v: Double, lo: Double, hi: Double): Double = min(hi, max(lo, v))

    fun hasEdits(s: EditState): Boolean {
        val a = s.adjust
        return s.rotation != Rotation.R0 || s.flipX || s.crop != null || s.filter != PhotoFilter.None ||
            a.brightness != 0.0 || a.contrast != 0.0 || a.saturation != 0.0 || a.blur != 0.0 || s.strokes.isNotEmpty()
    }

    // --- Geometry -----------------------------------------------------------------------------------------------

    /** The picture's size after rotating a quarter turn at a time. */
    fun rotatedSize(w: Int, h: Int, rotation: Rotation): Pair<Int, Int> =
        if (rotation == Rotation.R90 || rotation == Rotation.R270) h to w else w to h

    fun rotateBy(rotation: Rotation, quarterTurns: Int): Rotation {
        val quarters = (((rotation.degrees / 90 + quarterTurns) % 4) + 4) % 4
        return Rotation.entries[quarters]
    }

    /** A crop kept inside the picture and no smaller than [MIN_CROP] either way. */
    fun clampRect(r: Rect): Rect {
        val w = clamp(r.w, MIN_CROP, 1.0)
        val h = clamp(r.h, MIN_CROP, 1.0)
        return Rect(clamp(r.x, 0.0, 1 - w), clamp(r.y, 0.0, 1 - h), w, h)
    }

    /** The largest centred crop with this width-to-height ratio (null: the whole picture). */
    fun aspectCrop(aspect: Double?, imageW: Int, imageH: Int): Rect? {
        if (aspect == null || !(aspect > 0) || imageW <= 0 || imageH <= 0) return null
        val current = imageW.toDouble() / imageH
        if (abs(current - aspect) < 1e-3) return null
        if (current > aspect) {
            val w = aspect / current
            return Rect((1 - w) / 2, 0.0, w, 1.0)
        }
        val h = current / aspect
        return Rect(0.0, (1 - h) / 2, 1.0, h)
    }

    /** Drags a crop by a handle; [dx] and [dy] are fractions of the picture. Always returns a valid crop. */
    fun dragRect(r: Rect, handle: Handle, dx: Double, dy: Double): Rect {
        var x = r.x
        var y = r.y
        var w = r.w
        var h = r.h
        if (handle == Handle.Move) return clampRect(Rect(clamp(x + dx, 0.0, 1 - w), clamp(y + dy, 0.0, 1 - h), w, h))
        if ('w' in handle.wire) {
            val nx = clamp(x + dx, 0.0, x + w - MIN_CROP)
            w += x - nx
            x = nx
        }
        if ('e' in handle.wire) w = clamp(w + dx, MIN_CROP, 1 - x)
        if ('n' in handle.wire) {
            val ny = clamp(y + dy, 0.0, y + h - MIN_CROP)
            h += y - ny
            y = ny
        }
        if ('s' in handle.wire) h = clamp(h + dy, MIN_CROP, 1 - y)
        return clampRect(Rect(x, y, w, h))
    }

    /** Which part of the rotated picture ends up in the result, in pixels of the rotated picture. */
    class Pixels(val sx: Int, val sy: Int, val sw: Int, val sh: Int)

    fun cropPixels(imageW: Int, imageH: Int, s: EditState): Pixels {
        val (rw, rh) = rotatedSize(imageW, imageH, s.rotation)
        val c = s.crop ?: Rect(0.0, 0.0, 1.0, 1.0)
        return Pixels(jsRound(c.x * rw).toInt(), jsRound(c.y * rh).toInt(), max(1L, jsRound(c.w * rw)).toInt(), max(1L, jsRound(c.h * rh)).toInt())
    }

    /** The finished picture's size, scaled down so its long side is at most [maxSide]. */
    fun outputSize(imageW: Int, imageH: Int, s: EditState, maxSide: Int): Pair<Int, Int> {
        val p = cropPixels(imageW, imageH, s)
        val scale = min(1.0, maxSide.toDouble() / max(p.sw, p.sh))
        return max(1L, jsRound(p.sw * scale)).toInt() to max(1L, jsRound(p.sh * scale)).toInt()
    }

    // --- Colour -------------------------------------------------------------------------------------------------

    private class Preset(
        val brightness: Double = 1.0,
        val contrast: Double = 1.0,
        val saturate: Double = 1.0,
        val grayscale: Double = 0.0,
        val sepia: Double = 0.0,
        val hueRotate: Double = 0.0,
    )

    private fun preset(f: PhotoFilter): Preset = when (f) {
        PhotoFilter.None -> Preset()
        PhotoFilter.Vivid -> Preset(saturate = 1.45, contrast = 1.1)
        PhotoFilter.Warm -> Preset(sepia = 0.25, saturate = 1.2, hueRotate = -8.0, brightness = 1.03)
        PhotoFilter.Cool -> Preset(hueRotate = 12.0, saturate = 0.95, brightness = 1.02, contrast = 1.04)
        PhotoFilter.Fade -> Preset(contrast = 0.85, brightness = 1.08, saturate = 0.8)
        PhotoFilter.Mono -> Preset(grayscale = 1.0)
        PhotoFilter.Noir -> Preset(grayscale = 1.0, contrast = 1.3, brightness = 0.92)
        PhotoFilter.Sepia -> Preset(sepia = 0.9, contrast = 1.05)
    }

    /** The preset and the sliders together. Sliders run -100..100 around 1, blur 0..100 up to 1% of the height. */
    fun colorParams(s: EditState, outputHeight: Double = 1000.0): ColorParams {
        val p = preset(s.filter)
        val a = s.adjust
        // The slider is held to its range first, then scaled: brightness and contrast move less than saturation.
        fun slider(v: Double, strength: Double = 1.0) = 1 + clamp(v, -100.0, 100.0) * strength / 100
        return ColorParams(
            brightness = p.brightness * slider(a.brightness, 0.6),
            contrast = p.contrast * slider(a.contrast, 0.8),
            saturate = p.saturate * slider(a.saturation),
            grayscale = p.grayscale,
            sepia = p.sepia,
            hueRotate = p.hueRotate,
            blurPx = clamp(a.blur, 0.0, 100.0) / 100 * (outputHeight * 0.01),
        )
    }

    /** Rounds and holds to 0..255 the way a Uint8ClampedArray does (halves go to the even number). */
    private fun byte(v: Double): Int = if (v.isNaN() || v <= 0) 0 else if (v >= 255) 255 else Math.rint(v).toInt()

    /**
     * Colour changes done by hand on pixels (the web's `adjustPixels`: brightness, contrast, saturation, grayscale and sepia). [hue] adds the
     * hue rotation of the CSS filter, which the web's by-hand fallback leaves out; it is left off when checking against the web's vectors.
     * [rgba] is four bytes per pixel and changes in place.
     */
    fun adjustPixels(rgba: ByteArray, c: ColorParams, hue: Boolean = false) {
        val out = DoubleArray(3)
        var i = 0
        while (i + 3 < rgba.size) {
            adjustOne((rgba[i].toInt() and 0xff).toDouble(), (rgba[i + 1].toInt() and 0xff).toDouble(), (rgba[i + 2].toInt() and 0xff).toDouble(), c, hue, out)
            rgba[i] = byte(out[0]).toByte()
            rgba[i + 1] = byte(out[1]).toByte()
            rgba[i + 2] = byte(out[2]).toByte()
            i += 4
        }
    }

    /** The same on Android's pixels (alpha first, packed in an Int). */
    fun adjustArgb(pixels: IntArray, c: ColorParams, hue: Boolean = true) {
        val out = DoubleArray(3)
        for (i in pixels.indices) {
            val p = pixels[i]
            adjustOne(((p shr 16) and 0xff).toDouble(), ((p shr 8) and 0xff).toDouble(), (p and 0xff).toDouble(), c, hue, out)
            pixels[i] = (p and 0xff000000.toInt()) or (byte(out[0]) shl 16) or (byte(out[1]) shl 8) or byte(out[2])
        }
    }

    private fun adjustOne(r0: Double, g0: Double, b0: Double, c: ColorParams, hue: Boolean, out: DoubleArray) {
        var r = r0
        var g = g0
        var b = b0
        if (c.saturate != 1.0) {
            val l = 0.299 * r + 0.587 * g + 0.114 * b
            r = l + (r - l) * c.saturate
            g = l + (g - l) * c.saturate
            b = l + (b - l) * c.saturate
        }
        if (c.grayscale > 0) {
            val l = 0.299 * r + 0.587 * g + 0.114 * b
            r += (l - r) * c.grayscale
            g += (l - g) * c.grayscale
            b += (l - b) * c.grayscale
        }
        if (c.sepia > 0) {
            val tr = 0.393 * r + 0.769 * g + 0.189 * b
            val tg = 0.349 * r + 0.686 * g + 0.168 * b
            val tb = 0.272 * r + 0.534 * g + 0.131 * b
            r += (tr - r) * c.sepia
            g += (tg - g) * c.sepia
            b += (tb - b) * c.sepia
        }
        if (hue && c.hueRotate != 0.0) {
            val a = c.hueRotate * PI / 180
            val cs = cos(a)
            val sn = sin(a)
            val nr = (0.213 + cs * 0.787 - sn * 0.213) * r + (0.715 - cs * 0.715 - sn * 0.715) * g + (0.072 - cs * 0.072 + sn * 0.928) * b
            val ng = (0.213 - cs * 0.213 + sn * 0.143) * r + (0.715 + cs * 0.285 + sn * 0.140) * g + (0.072 - cs * 0.072 - sn * 0.283) * b
            val nb = (0.213 - cs * 0.213 - sn * 0.787) * r + (0.715 - cs * 0.715 + sn * 0.715) * g + (0.072 + cs * 0.928 + sn * 0.072) * b
            r = nr
            g = ng
            b = nb
        }
        if (c.contrast != 1.0) {
            r = (r - 128) * c.contrast + 128
            g = (g - 128) * c.contrast + 128
            b = (b - 128) * c.contrast + 128
        }
        if (c.brightness != 1.0) {
            r *= c.brightness
            g *= c.brightness
            b *= c.brightness
        }
        out[0] = r
        out[1] = g
        out[2] = b
    }

    /**
     * A blur of about [radius] pixels: three box passes along each side, which is close to a Gaussian one. Not the same pixels the browser's
     * blur makes (the web draws with the canvas's own), so this is checked for what it must do, not against the web.
     */
    fun blurArgb(pixels: IntArray, width: Int, height: Int, radius: Double) {
        if (radius < 0.5 || width < 1 || height < 1) return
        // Three box passes of width w approximate a Gaussian of sigma = radius: w = sqrt(12 * sigma^2 / 3 + 1).
        val box = max(1, kotlin.math.round(kotlin.math.sqrt(4 * radius * radius + 1) / 2).toInt())
        val a = IntArray(pixels.size)
        val tmp = IntArray(pixels.size)
        System.arraycopy(pixels, 0, a, 0, pixels.size)
        repeat(3) {
            boxPass(a, tmp, width, height, box, horizontal = true)
            boxPass(tmp, a, width, height, box, horizontal = false)
        }
        System.arraycopy(a, 0, pixels, 0, pixels.size)
    }

    private fun boxPass(src: IntArray, dst: IntArray, width: Int, height: Int, radius: Int, horizontal: Boolean) {
        val lines = if (horizontal) height else width
        val length = if (horizontal) width else height
        for (line in 0 until lines) {
            var sa = 0L
            var sr = 0L
            var sg = 0L
            var sb = 0L
            fun at(i: Int): Int = if (horizontal) src[line * width + i] else src[i * width + line]
            // The window [i - radius, i + radius], with the edge pixel repeated beyond the picture.
            for (k in -radius..radius) {
                val p = at(k.coerceIn(0, length - 1))
                sa += (p ushr 24) and 0xff; sr += (p shr 16) and 0xff; sg += (p shr 8) and 0xff; sb += p and 0xff
            }
            val count = (2 * radius + 1).toLong()
            for (i in 0 until length) {
                val value = ((sa / count).toInt() shl 24) or ((sr / count).toInt() shl 16) or ((sg / count).toInt() shl 8) or (sb / count).toInt()
                if (horizontal) dst[line * width + i] = value else dst[i * width + line] = value
                val out = at((i - radius).coerceIn(0, length - 1))
                val inn = at((i + radius + 1).coerceIn(0, length - 1))
                sa += ((inn ushr 24) and 0xff) - ((out ushr 24) and 0xff)
                sr += ((inn shr 16) and 0xff) - ((out shr 16) and 0xff)
                sg += ((inn shr 8) and 0xff) - ((out shr 8) and 0xff)
                sb += (inn and 0xff) - (out and 0xff)
            }
        }
    }

    // --- Drawing ------------------------------------------------------------------------------------------------

    fun newStroke(color: StoryColor, size: Double, eraser: Boolean, at: Point): Stroke =
        Stroke(color, clamp(size, MIN_PEN, MAX_PEN), eraser, listOf(Point(clamp(at.x, 0.0, 1.0), clamp(at.y, 0.0, 1.0))))

    /** Adds a point to a stroke, keeping it within bounds and a sensible size. */
    fun extendStroke(stroke: Stroke, p: Point): Stroke {
        if (stroke.points.size >= MAX_POINTS_PER_STROKE) return stroke
        val last = stroke.points.lastOrNull()
        val next = Point(clamp(p.x, 0.0, 1.0), clamp(p.y, 0.0, 1.0))
        // Skip points too close to the last one: they only make the file heavier.
        if (last != null && hypot(next.x - last.x, next.y - last.y) < 0.0015) return stroke
        return stroke.copy(points = stroke.points + next)
    }

    fun addStroke(s: EditState, stroke: Stroke): EditState = s.copy(strokes = (s.strokes + stroke).takeLast(MAX_STROKES))

    /** Drawing is on the finished picture, so a change of crop, turn or flip would leave it in the wrong place: it is cleared. */
    fun withGeometry(s: EditState, rotation: Rotation = s.rotation, flipX: Boolean = s.flipX, crop: Rect? = s.crop): EditState =
        s.copy(rotation = rotation, flipX = flipX, crop = crop, strokes = emptyList())
}

/** Undo and redo for the editors: a plain value at each step, nothing changed in place (port of lib/studio/history.ts). */
class EditHistory<T>(val past: List<T>, val present: T, val future: List<T>) {
    val canUndo: Boolean get() = past.isNotEmpty()
    val canRedo: Boolean get() = future.isNotEmpty()

    /** A new step. Anything that could have been redone is gone; the oldest steps drop off past [limit]. */
    fun push(value: T, limit: Int = MAX_UNDO): EditHistory<T> {
        if (value === present) return this
        var steps = past + present
        while (steps.size > limit) steps = steps.drop(1)
        return EditHistory(steps, value, emptyList())
    }

    /** The present replaced without a step of its own (a slider being dragged is one step, not fifty). */
    fun replace(value: T): EditHistory<T> = EditHistory(past, value, future)

    fun undo(): EditHistory<T> = if (!canUndo) this else EditHistory(past.dropLast(1), past.last(), listOf(present) + future)

    fun redo(): EditHistory<T> = if (!canRedo) this else EditHistory(past + present, future.first(), future.drop(1))

    companion object {
        const val MAX_UNDO = 60
        fun <T> start(value: T) = EditHistory(emptyList(), value, emptyList())
    }
}
