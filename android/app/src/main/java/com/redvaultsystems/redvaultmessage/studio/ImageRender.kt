// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.studio

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.Rect as PixelRect
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import java.io.ByteArrayOutputStream

/**
 * Draws an [EditState] with Android's bitmaps: turn and flip, crop, colour, blur, then the drawing on its own layer (so the eraser takes away
 * drawing, never the photo). The same order the web draws in (ui/src/lib/studio/imageEdit.ts `drawEdit`). The maths behind each step is in
 * [ImageEdit] and pinned to the web's; this is only the drawing, and is checked by eye and by the tests that run on a device.
 */
object ImageRender {
    /** The pen colours as real colours (the drawing is baked into the picture), matching the story colour tokens. */
    fun penColor(c: StoryColor): Int = when (c) {
        StoryColor.White -> 0xFFFFFFFF.toInt()
        StoryColor.Black -> 0xFF111111.toInt()
        StoryColor.Red -> 0xFFEF4444.toInt()
        StoryColor.Orange -> 0xFFF97316.toInt()
        StoryColor.Yellow -> 0xFFFACC15.toInt()
        StoryColor.Green -> 0xFF22C55E.toInt()
        StoryColor.Teal -> 0xFF14B8A6.toInt()
        StoryColor.Blue -> 0xFF3B82F6.toInt()
        StoryColor.Purple -> 0xFFA855F7.toInt()
        StoryColor.Pink -> 0xFFEC4899.toInt()
    }

    /** The picture drawn turned and flipped, as big as the turned picture (the flip mirrors the finished picture, after the turn). */
    private fun turned(src: Bitmap, s: EditState): Bitmap {
        if (s.rotation == Rotation.R0 && !s.flipX) return src
        val (rw, rh) = ImageEdit.rotatedSize(src.width, src.height, s.rotation)
        val matrix = Matrix().apply {
            postTranslate(-src.width / 2f, -src.height / 2f)
            postRotate(s.rotation.degrees.toFloat())
            if (s.flipX) postScale(-1f, 1f)
            postTranslate(rw / 2f, rh / 2f)
        }
        val out = Bitmap.createBitmap(rw, rh, Bitmap.Config.ARGB_8888)
        Canvas(out).drawBitmap(src, matrix, Paint(Paint.FILTER_BITMAP_FLAG))
        return out
    }

    /** The edited picture, its long side at most [maxSide] (stories use 1920; the live preview uses less). */
    fun draw(src: Bitmap, s: EditState, maxSide: Int): Bitmap {
        val turned = turned(src, s)
        val crop = ImageEdit.cropPixels(src.width, src.height, s)
        val (ow, oh) = ImageEdit.outputSize(src.width, src.height, s, maxSide)
        val out = Bitmap.createBitmap(ow, oh, Bitmap.Config.ARGB_8888)
        Canvas(out).drawBitmap(
            turned,
            PixelRect(crop.sx, crop.sy, (crop.sx + crop.sw).coerceAtMost(turned.width), (crop.sy + crop.sh).coerceAtMost(turned.height)),
            PixelRect(0, 0, ow, oh),
            Paint(Paint.FILTER_BITMAP_FLAG),
        )
        if (turned !== src) turned.recycle()
        val params = ImageEdit.colorParams(s, oh.toDouble())
        if (!params.neutral) {
            val pixels = IntArray(ow * oh)
            out.getPixels(pixels, 0, ow, 0, 0, ow, oh)
            ImageEdit.adjustArgb(pixels, params, hue = true)
            if (params.blurPx > 0.05) ImageEdit.blurArgb(pixels, ow, oh, params.blurPx)
            out.setPixels(pixels, 0, ow, 0, 0, ow, oh)
        }
        if (s.strokes.isNotEmpty()) {
            val layer = Bitmap.createBitmap(ow, oh, Bitmap.Config.ARGB_8888)
            drawStrokes(Canvas(layer), s.strokes, ow, oh)
            Canvas(out).drawBitmap(layer, 0f, 0f, null)
            layer.recycle()
        }
        return out
    }

    /** Draws strokes onto a canvas the size of the finished picture. */
    fun drawStrokes(canvas: Canvas, strokes: List<Stroke>, w: Int, h: Int) {
        for (stroke in strokes) {
            val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = penColor(stroke.color)
                strokeCap = Paint.Cap.ROUND
                strokeJoin = Paint.Join.ROUND
                strokeWidth = (stroke.size * w).toFloat()
                xfermode = if (stroke.eraser) PorterDuffXfermode(PorterDuff.Mode.CLEAR) else null
            }
            val points = stroke.points
            if (points.size == 1) {
                paint.style = Paint.Style.FILL
                canvas.drawCircle((points[0].x * w).toFloat(), (points[0].y * h).toFloat(), (stroke.size * w / 2).toFloat(), paint)
                continue
            }
            paint.style = Paint.Style.STROKE
            val path = Path().apply {
                moveTo((points[0].x * w).toFloat(), (points[0].y * h).toFloat())
                for (i in 1 until points.size) lineTo((points[i].x * w).toFloat(), (points[i].y * h).toFloat())
            }
            canvas.drawPath(path, paint)
        }
    }

    /** The edited picture as a JPEG: a fresh file, so none of the original's metadata (location, camera, time) survives. */
    fun renderJpeg(src: Bitmap, s: EditState, maxSide: Int): Triple<ByteArray, Int, Int> {
        val bitmap = draw(src, s, maxSide)
        try {
            // A transparent corner (the picture's own transparency, or a drawing erased to nothing) becomes black in a JPEG; paint on black first.
            val flat = Bitmap.createBitmap(bitmap.width, bitmap.height, Bitmap.Config.ARGB_8888)
            Canvas(flat).apply { drawColor(0xFF000000.toInt()); drawBitmap(bitmap, 0f, 0f, null) }
            val out = ByteArrayOutputStream()
            if (!flat.compress(Bitmap.CompressFormat.JPEG, 90, out)) throw IllegalStateException("That photo couldn't be prepared.")
            val w = flat.width
            val h = flat.height
            flat.recycle()
            return Triple(out.toByteArray(), w, h)
        } finally {
            bitmap.recycle()
        }
    }
}
