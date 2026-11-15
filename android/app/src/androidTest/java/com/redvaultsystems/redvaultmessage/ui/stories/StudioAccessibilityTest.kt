// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import android.graphics.Bitmap
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.SemanticsNodeInteraction
import androidx.compose.ui.test.hasClickAction
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.redvaultsystems.redvaultmessage.data.PreparedStoryMedia
import com.redvaultsystems.redvaultmessage.data.StoryFriend
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream

/**
 * What a screen reader is given by the new studio screens: every control that can be pressed, typed into or moved has something to say about
 * itself (a label, a description, or the words inside it). This reads the same merged semantics tree TalkBack reads, so a control with
 * nothing to announce is found here. It is not a TalkBack run and does not judge touch-target sizes or colour contrast.
 */
@RunWith(AndroidJUnit4::class)
class StudioAccessibilityTest {
    @get:Rule val rule = createComposeRule()

    private val controls: SemanticsMatcher =
        hasClickAction() or hasSetTextAction() or SemanticsMatcher("has a range (a slider)") { n -> n.config.getOrNull(SemanticsProperties.ProgressBarRangeInfo) != null }

    /** The words a reader would speak for a node: its description, its text, its state description, or (for a clickable group) what is inside it. */
    private fun announced(interaction: SemanticsNodeInteraction): String {
        val c = interaction.fetchSemanticsNode().config
        val parts = ArrayList<String>()
        c.getOrNull(SemanticsProperties.ContentDescription)?.let { parts.addAll(it) }
        c.getOrNull(SemanticsProperties.Text)?.let { parts.addAll(it.map { t -> t.text }) }
        c.getOrNull(SemanticsProperties.EditableText)?.let { parts.add(it.text) }
        c.getOrNull(SemanticsProperties.StateDescription)?.let { parts.add(it) }
        return parts.joinToString(" ").trim()
    }

    private fun assertEveryControlSpeaks(screen: String) {
        rule.waitForIdle()
        val nodes = rule.onAllNodes(controls, useUnmergedTree = false)
        val count = nodes.fetchSemanticsNodes().size
        assertTrue("$screen: found no controls at all", count > 0)
        val silent = (0 until count).filter { announced(nodes[it]).isEmpty() }
        assertTrue("$screen: $count controls, ${silent.size} with nothing to announce: ${silent.map { nodes[it].fetchSemanticsNode().boundsInRoot }}", silent.isEmpty())
    }

    private fun photo(): PreparedStoryMedia {
        val bitmap = Bitmap.createBitmap(64, 48, Bitmap.Config.ARGB_8888).apply { eraseColor(0xFF3366CC.toInt()) }
        val out = ByteArrayOutputStream()
        bitmap.compress(Bitmap.CompressFormat.JPEG, 90, out)
        val bytes = out.toByteArray()
        return PreparedStoryMedia(StoryKind.Image, "image/jpeg", 64, 48, null, bytes.size.toLong()) { ByteArrayInputStream(bytes) }
    }

    @Test
    fun thePhotoEditorsControlsAreAllLabelledInEveryTab() {
        rule.setContent { RedvaultTheme { PhotoEditorScreen(photo(), onDone = {}, onCancel = {}) } }
        rule.waitUntil(5_000) { rule.onAllNodes(hasText("Crop")).fetchSemanticsNodes().isNotEmpty() }
        assertEveryControlSpeaks("photo editor, crop")
        for (tab in listOf("Adjust", "Filters", "Draw")) {
            rule.onNodeWithText(tab).performClick()
            assertEveryControlSpeaks("photo editor, $tab")
        }
    }

    @Test
    fun theVoiceStudiosFirstScreenAndTheSubtitleEditorAreLabelled() {
        rule.setContent { RedvaultTheme { VoiceStudio(start = null, onDone = {}, onCancel = {}) } }
        assertEveryControlSpeaks("voice studio, choose")
    }

    @Test
    fun theSubtitleEditorAndTheStudiosDialogsAreLabelled() {
        rule.setContent {
            RedvaultTheme {
                CaptionEditorDialog(
                    captions = listOf(com.redvaultsystems.redvaultmessage.data.model.StoryCaption(500, 2000, "First"), com.redvaultsystems.redvaultmessage.data.model.StoryCaption(2500, 4000, "Second")),
                    durationMs = 5000, onChange = {}, onClose = {},
                )
            }
        }
        assertEveryControlSpeaks("subtitle editor")
    }

    private val friends = listOf(StoryFriend("00000000-0000-4000-8000-0000000000a1", "ada", null, ByteArray(32)), StoryFriend("00000000-0000-4000-8000-0000000000b2", "bob", null, ByteArray(32)))

    @Test
    fun theWordsDialogIsLabelled() {
        rule.setContent { RedvaultTheme { OverlayDialog(null, onDone = {}, onDismiss = {}) } }
        assertEveryControlSpeaks("words dialog")
    }

    @Test
    fun theStickerKindListAndTheFormsAreLabelled() {
        rule.setContent { RedvaultTheme { StickerDialog(friends, interactive = true, onDone = {}, onDismiss = {}) } }
        assertEveryControlSpeaks("sticker kinds")
        rule.onNodeWithText("Quiz").performClick()
        assertEveryControlSpeaks("quiz form")
    }

    @Test
    fun thePeopleDialogIsLabelled() {
        rule.setContent { RedvaultTheme { PeopleDialog(friends, hidden = emptySet(), chosen = emptySet(), onDone = {}, onDismiss = {}) } }
        assertEveryControlSpeaks("people dialog")
    }

    @Test
    fun theCollageScreenIsLabelledWithAndWithoutPhotos() {
        val photos = listOf("A", "B", "C").map { name -> name to Bitmap.createBitmap(60, 100, Bitmap.Config.ARGB_8888).apply { eraseColor(0xFF3366CC.toInt()) } }
        rule.setContent { RedvaultTheme { CollageScreen(onDone = {}, onCancel = {}, startWith = photos) } }
        assertEveryControlSpeaks("collage, three photos")
    }

    @Test
    fun theCollageScreenIsLabelledBeforeAnyPhotoIsChosen() {
        rule.setContent { RedvaultTheme { CollageScreen(onDone = {}, onCancel = {}) } }
        assertEveryControlSpeaks("collage, empty")
    }
}
