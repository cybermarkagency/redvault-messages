package com.redvaultsystems.redvaultmessage.identity

import org.junit.Assert.assertEquals
import org.junit.Test

class GlyphTest {
    private fun render(seed: String): String =
        Glyph.generate(seed).joinToString("\n") { row ->
            row.joinToString("") { cell ->
                when {
                    !cell.filled -> "."
                    cell.tone == GlyphTone.Accent -> "A"
                    else -> "T"
                }
            }
        }

    @Test
    fun matchesWebClientOutput() {
        // Produced by ui/src/lib/glyph.ts's generateKeyGlyph("quiet-harbor·a1b2").
        val expected = listOf(".A.A.", "..T..", ".A.A.", ".AAA.", "TAAAT").joinToString("\n")
        assertEquals(expected, render("quiet-harbor·a1b2"))
    }

    @Test
    fun isMirroredLeftRight() {
        val grid = Glyph.generate("any-seed")
        for (row in grid) {
            assertEquals(row[0], row[4])
            assertEquals(row[1], row[3])
        }
    }
}
