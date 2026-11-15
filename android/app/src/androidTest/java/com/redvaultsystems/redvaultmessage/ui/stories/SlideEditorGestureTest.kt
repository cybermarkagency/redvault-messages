// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.pinch
import androidx.compose.ui.test.center
import androidx.compose.ui.test.swipe
import androidx.compose.ui.unit.dp
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.redvaultsystems.redvaultmessage.data.SlideMode
import com.redvaultsystems.redvaultmessage.data.model.LocationSticker
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryOverlay
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * The composer's gestures with real touch input on a device: dragging an item moves it, pinching two fingers apart or together sizes it,
 * turning two fingers turns it, and what is placed stays inside the limits (the same limits the viewers read with). The slide is shown the
 * way the composer shows it; its state is held here.
 */
@RunWith(AndroidJUnit4::class)
class SlideEditorGestureTest {
    @get:Rule val rule = createComposeRule()

    private var slide by mutableStateOf(
        SlideUi(
            key = 0,
            mode = SlideMode.Text,
            text = "Pinch here",
            overlays = listOf(StoryOverlay(false, "Hello", 0.5, 0.5, 1.0, 0.0, StoryColor.White, false, StoryFont.Classic, null)),
            stickers = listOf(LocationSticker("place1234", 0.5, 0.8, 1.0, 0.0, "Lisbon")),
        ),
    )
    private var selected by mutableStateOf<Selected?>(null)

    private fun show() = rule.setContent {
        RedvaultTheme {
            Box(Modifier.fillMaxWidth().padding(8.dp)) {
                SlideEditor(
                    slide = slide,
                    selected = selected,
                    friends = emptyMap(),
                    onSelect = { selected = it },
                    onOverlay = { i, o -> slide = slide.copy(overlays = slide.overlays.mapIndexed { j, x -> if (j == i) o else x }) },
                    onSticker = { s -> slide = slide.copy(stickers = slide.stickers.map { if (it.id == s.id) s else it }) },
                    modifier = Modifier.fillMaxWidth(0.8f),
                )
            }
        }
    }

    private val words get() = slide.overlays[0]
    private val place get() = slide.stickers[0]

    @Test
    fun draggingMovesAnItemAndSelectsIt() {
        show()
        rule.onNodeWithText("Hello").performTouchInput { swipe(center, center + Offset(0f, -120f), 300) }
        rule.waitForIdle()
        assertEquals("moved up", true, words.y < 0.45)
        assertEquals("x stays about where it was", 0.5, words.x, 0.08)
        assertEquals(Selected.Overlay(0), selected)
    }

    @Test
    fun pinchingTwoFingersApartSizesAnOverlayUpAndTogetherSizesItDown() {
        show()
        rule.onNodeWithText("Hello").performTouchInput {
            pinch(center + Offset(-10f, 0f), center + Offset(-110f, 0f), center + Offset(10f, 0f), center + Offset(110f, 0f), 400)
        }
        rule.waitForIdle()
        assertTrue("sized up: ${words.scale}", words.scale > 1.4)
        assertTrue("within the limit of 4: ${words.scale}", words.scale <= 4.0)
        val big = words.scale
        // The item is already drawn bigger, and the fingers are placed on the screen, not on the unscaled item: divide by its size.
        val k = words.scale.toFloat()
        rule.onNodeWithText("Hello").performTouchInput {
            pinch(center + Offset(-110f / k, 0f), center + Offset(-10f / k, 0f), center + Offset(110f / k, 0f), center + Offset(10f / k, 0f), 400)
        }
        rule.waitForIdle()
        assertTrue("sized down from $big to ${words.scale}", words.scale < big - 0.3)
        assertTrue("never below 0.5: ${words.scale}", words.scale >= 0.5)
    }

    @Test
    fun turningTwoFingersTurnsTheItemByAboutThatMuch() {
        show()
        rule.onNodeWithText("Hello").performTouchInput {
            // The fingers start left and right of the middle and end above and below it: a quarter turn.
            pinch(center + Offset(-80f, 0f), center + Offset(0f, -80f), center + Offset(80f, 0f), center + Offset(0f, 80f), 500)
        }
        rule.waitForIdle()
        val r = words.rotation
        assertTrue("turned about a quarter: $r", r in 45.0..135.0 || r in 225.0..315.0)
        assertTrue("stays between 0 and 360: $r", r >= 0.0 && r < 360.0)
    }

    @Test
    fun aStickerIsSizedWithinItsOwnLimitsOfTwoThirdsAndTwoAndAHalf() {
        show()
        rule.onNodeWithText("📍 Lisbon").performTouchInput {
            pinch(center + Offset(-5f, 0f), center + Offset(-400f, 0f), center + Offset(5f, 0f), center + Offset(400f, 0f), 400)
        }
        rule.waitForIdle()
        assertTrue("sticker up, held at 2.5: ${place.scale}", place.scale > 1.5 && place.scale <= 2.5)
        val k = place.scale.toFloat()
        rule.onNodeWithText("📍 Lisbon").performTouchInput {
            pinch(center + Offset(-300f / k, 0f), center + Offset(-2f / k, 0f), center + Offset(300f / k, 0f), center + Offset(2f / k, 0f), 400)
        }
        rule.waitForIdle()
        assertTrue("sticker down, held at 0.6: ${place.scale}", place.scale >= 0.6 && place.scale < 1.2)
    }

    @Test
    fun anItemCanNeverBeDraggedOffThePicture() {
        show()
        rule.onNodeWithText("Hello").performTouchInput { swipe(center, center + Offset(5000f, -5000f), 300) }
        rule.waitForIdle()
        assertTrue("x in 0..1: ${words.x}", words.x in 0.0..1.0)
        assertTrue("y in 0..1: ${words.y}", words.y in 0.0..1.0)
    }
}
