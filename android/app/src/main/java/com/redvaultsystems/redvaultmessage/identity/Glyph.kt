package com.redvaultsystems.redvaultmessage.identity

// Port of ui/src/lib/glyph.ts. Must stay bit-identical to it (pinned by
// GlyphTest), so the same handle draws the same glyph on web and Android.
// Kotlin Int arithmetic wraps exactly like JS's `| 0` / Math.imul, and
// `ushr` is JS's `>>>`.

enum class GlyphTone { Accent, Text3 }

data class GlyphCell(val filled: Boolean, val tone: GlyphTone)

/** 5x5, indexed [row][col]. */
typealias GlyphGrid = List<List<GlyphCell>>

object Glyph {
    private const val GRID_SIZE = 5
    private const val CENTER_COL = 2
    private const val FILL_PROBABILITY = 0.5
    private const val ACCENT_PROBABILITY = 0.72

    /** FNV-1a over UTF-16 code units, like JS charCodeAt. */
    fun fnv1aHash(input: String): Int {
        var hash = 0x811c9dc5L.toInt()
        for (ch in input) {
            hash = hash xor ch.code
            hash *= 0x01000193
        }
        return hash
    }

    /** mulberry32: small, fast, deterministic PRNG from a 32-bit seed. Returns values in [0, 1). */
    fun mulberry32(seed: Int): () -> Double {
        var a = seed
        return {
            a += 0x6d2b79f5
            var t = (a xor (a ushr 15)) * (1 or a)
            t = (t + (t xor (t ushr 7)) * (61 or t)) xor t
            ((t xor (t ushr 14)).toLong() and 0xffffffffL) / 4294967296.0
        }
    }

    /**
     * A deterministic 5x5 grid seeded from [seed], mirrored left-right so it
     * reads as a symmetric mark. Filled cells are ~72% accent, ~28% text-3.
     */
    fun generate(seed: String): GlyphGrid {
        val rng = mulberry32(fnv1aHash(seed))
        val grid = Array(GRID_SIZE) { arrayOfNulls<GlyphCell>(GRID_SIZE) }
        for (row in 0 until GRID_SIZE) {
            for (col in 0..CENTER_COL) {
                val filled = rng() < FILL_PROBABILITY
                val tone = if (rng() < ACCENT_PROBABILITY) GlyphTone.Accent else GlyphTone.Text3
                val cell = GlyphCell(filled, tone)
                grid[row][col] = cell
                grid[row][GRID_SIZE - 1 - col] = cell
            }
        }
        return grid.map { row -> row.map { it!! } }
    }
}
