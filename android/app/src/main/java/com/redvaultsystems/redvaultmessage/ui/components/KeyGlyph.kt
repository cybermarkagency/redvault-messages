package com.redvaultsystems.redvaultmessage.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.redvaultsystems.redvaultmessage.identity.Glyph
import com.redvaultsystems.redvaultmessage.identity.GlyphTone
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/** Port of ui/src/components/KeyGlyph.tsx: 12% padding, 8% gaps, 22% corner radius. */
@Composable
fun KeyGlyph(seed: String, size: Dp = 40.dp, modifier: Modifier = Modifier) {
    val grid = remember(seed) { Glyph.generate(seed) }
    val colors = RedvaultTheme.colors
    Box(
        modifier = modifier
            .size(size)
            .background(colors.surface2, RoundedCornerShape(22))
            .padding(size * 0.12f)
            .semantics { contentDescription = "Key glyph for $seed" },
    ) {
        Canvas(modifier = Modifier.size(size * 0.76f)) {
            val gap = this.size.width * 0.08f
            val cell = (this.size.width - gap * 4) / 5
            val radius = CornerRadius(2.dp.toPx())
            grid.forEachIndexed { row, cells ->
                cells.forEachIndexed { col, c ->
                    if (c.filled) {
                        drawRoundRect(
                            color = if (c.tone == GlyphTone.Accent) colors.accent else colors.text3,
                            topLeft = Offset(col * (cell + gap), row * (cell + gap)),
                            size = Size(cell, cell),
                            cornerRadius = radius,
                        )
                    }
                }
            }
        }
    }
}
