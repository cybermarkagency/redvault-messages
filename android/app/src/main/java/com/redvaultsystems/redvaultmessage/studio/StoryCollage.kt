// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.studio

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import java.io.ByteArrayOutputStream
import kotlin.math.max
import kotlin.math.roundToInt

/** A cell as fractions (0 to 1) of the whole picture. */
data class CollageCell(val x: Double, val y: Double, val w: Double, val h: Double)

data class CollageLayout(val id: String, val label: String, val cells: List<CollageCell>)

/** A cell in pixels. */
data class CellRect(val x: Double, val y: Double, val w: Double, val h: Double)

/** The part of a picture to draw so it fills a cell without stretching. */
data class CoverPart(val sx: Double, val sy: Double, val sw: Double, val sh: Double)

/** One of the four backgrounds a collage can sit on (the web's list). */
class CollageBackground(val label: String, val argb: Int)

/**
 * Collages: two to four photos in one picture, laid out and drawn on this phone. Port of ui/src/lib/studio/collage.ts: the layouts and the
 * maths (cell sizes, how a photo fills its cell) are the web's and are pinned to its vectors; the result is a normal story photo, a fresh JPEG.
 */
object StoryCollage {
    const val MAX_PHOTOS = 4
    const val WIDTH = 1080
    const val HEIGHT = 1920
    const val DEFAULT_GAP = 12.0
    const val MAX_GAP = 40

    val BACKGROUNDS = listOf(
        CollageBackground("Dark", 0xFF111111.toInt()),
        CollageBackground("Light", 0xFFF5F5F4.toInt()),
        CollageBackground("Warm", 0xFF7C2D12.toInt()),
        CollageBackground("Cool", 0xFF1E3A8A.toInt()),
    )

    private fun grid(cols: Int, rows: Int): List<CollageCell> = buildList {
        for (r in 0 until rows) for (c in 0 until cols) add(CollageCell(c.toDouble() / cols, r.toDouble() / rows, 1.0 / cols, 1.0 / rows))
    }

    val LAYOUTS = listOf(
        CollageLayout("stack2", "Two, stacked", grid(1, 2)),
        CollageLayout("side2", "Two, side by side", grid(2, 1)),
        CollageLayout("stack3", "Three, stacked", grid(1, 3)),
        CollageLayout(
            "big1small2", "One big, two small",
            listOf(CollageCell(0.0, 0.0, 1.0, 0.6), CollageCell(0.0, 0.6, 0.5, 0.4), CollageCell(0.5, 0.6, 0.5, 0.4)),
        ),
        CollageLayout(
            "left1right2", "One tall, two short",
            listOf(CollageCell(0.0, 0.0, 0.5, 1.0), CollageCell(0.5, 0.0, 0.5, 0.5), CollageCell(0.5, 0.5, 0.5, 0.5)),
        ),
        CollageLayout("grid4", "Four", grid(2, 2)),
    )

    /** The layouts that take exactly [count] photos. */
    fun layoutsFor(count: Int): List<CollageLayout> = LAYOUTS.filter { it.cells.size == count }

    /** Cells in pixels, each pulled in by half the gap on every side so neighbours are [gap] apart (and the edges are too). */
    fun cellRects(layout: CollageLayout, width: Double, height: Double, gap: Double): List<CellRect> {
        val half = gap / 2
        return layout.cells.map { c ->
            CellRect(c.x * width + half, c.y * height + half, max(1.0, c.w * width - gap), max(1.0, c.h * height - gap))
        }
    }

    /** The part of a picture to draw so it fills a cell without stretching (the middle, cropped to fit). */
    fun fitCover(imageW: Double, imageH: Double, cellW: Double, cellH: Double): CoverPart {
        val cellAspect = cellW / cellH
        val imageAspect = imageW / imageH
        if (imageAspect > cellAspect) {
            val sw = imageH * cellAspect
            return CoverPart((imageW - sw) / 2, 0.0, sw, imageH)
        }
        val sh = imageW / cellAspect
        return CoverPart(0.0, (imageH - sh) / 2, imageW, sh)
    }

    /** The photos drawn into the layout (the editor's live preview uses a smaller size). */
    fun draw(images: List<Bitmap>, layout: CollageLayout, width: Int = WIDTH, height: Int = HEIGHT, gap: Double = DEFAULT_GAP, background: Int = BACKGROUNDS[0].argb): Bitmap {
        require(images.size == layout.cells.size) { "This layout needs ${layout.cells.size} photos." }
        val out = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(out)
        canvas.drawColor(background)
        val paint = Paint(Paint.FILTER_BITMAP_FLAG)
        val rects = cellRects(layout, width.toDouble(), height.toDouble(), gap)
        images.forEachIndexed { i, image ->
            val r = rects[i]
            val f = fitCover(image.width.toDouble(), image.height.toDouble(), r.w, r.h)
            val source = Rect(
                f.sx.roundToInt().coerceIn(0, image.width - 1),
                f.sy.roundToInt().coerceIn(0, image.height - 1),
                (f.sx + f.sw).roundToInt().coerceIn(1, image.width),
                (f.sy + f.sh).roundToInt().coerceIn(1, image.height),
            )
            canvas.drawBitmap(image, source, RectF(r.x.toFloat(), r.y.toFloat(), (r.x + r.w).toFloat(), (r.y + r.h).toFloat()), paint)
        }
        return out
    }

    /** The collage as a JPEG: a fresh file, so none of the photos' own metadata (location, camera, time) survives. */
    fun renderJpeg(images: List<Bitmap>, layout: CollageLayout, gap: Double, background: Int): Triple<ByteArray, Int, Int> {
        val bitmap = draw(images, layout, WIDTH, HEIGHT, gap, background)
        try {
            val out = ByteArrayOutputStream()
            if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 90, out)) throw IllegalStateException("That collage couldn't be made.")
            return Triple(out.toByteArray(), bitmap.width, bitmap.height)
        } finally {
            bitmap.recycle()
        }
    }
}
