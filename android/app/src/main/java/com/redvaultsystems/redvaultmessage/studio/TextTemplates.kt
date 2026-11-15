// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.studio

import com.redvaultsystems.redvaultmessage.data.model.CountdownSticker
import com.redvaultsystems.redvaultmessage.data.model.PollSticker
import com.redvaultsystems.redvaultmessage.data.model.QuestionSticker
import com.redvaultsystems.redvaultmessage.data.model.ResultsVisibility
import com.redvaultsystems.redvaultmessage.data.model.SliderSticker
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryOverlay
import com.redvaultsystems.redvaultmessage.data.model.StorySticker
import com.redvaultsystems.redvaultmessage.data.model.StoryStickerParser
import kotlinx.serialization.json.JsonArray

/** What a template starts a text story from (what [TextTemplates.apply] returns): the words, the look, the stickers and the emoji. */
class AppliedTemplate(
    val text: String,
    val background: StoryBackground,
    val font: StoryFont,
    val stickers: List<StorySticker>,
    val overlays: List<StoryOverlay>,
)

/**
 * A template for a text story: a look (background, font) and often a sticker to fill in, as starting points; everything stays editable and
 * nothing is fetched from anywhere. Port of ui/src/lib/studio/textTemplates.ts, pinned to the web's own output.
 */
class TextTemplate(
    val id: String,
    val label: String,
    val text: String,
    val background: StoryBackground,
    val font: StoryFont,
    /** Each makes a sticker given its fresh id and "now" (epoch ms); the fields to fill in start with the template's words. */
    val stickers: List<(id: String, nowMs: Long) -> StorySticker>,
    val overlays: List<StoryOverlay>,
)

object TextTemplates {
    private const val X = 0.5
    private const val STICKER_Y = 0.62
    private const val WEEK_MS = 7 * 86_400_000L

    private fun emoji(text: String, x: Double, y: Double, scale: Double = 1.0) =
        StoryOverlay(true, text, x, y, scale, 0.0, StoryColor.White, false, StoryFont.Classic, null)

    val ALL: List<TextTemplate> = listOf(
        TextTemplate("plain", "Plain", "", StoryBackground.Sunset, StoryFont.Classic, emptyList(), emptyList()),
        TextTemplate(
            "this-or-that", "This or that", "This or that?", StoryBackground.Ocean, StoryFont.Strong,
            listOf({ id, _ -> PollSticker(id, X, STICKER_Y, 1.0, 0.0, "Which one?", listOf("This", "That"), null, ResultsVisibility.Everyone) }),
            emptyList(),
        ),
        TextTemplate(
            "ask-me", "Ask me anything", "Ask me anything", StoryBackground.Berry, StoryFont.Modern,
            listOf({ id, _ -> QuestionSticker(id, X, STICKER_Y, 1.0, 0.0, "Ask me anything", false) }),
            emptyList(),
        ),
        TextTemplate(
            "mood", "Mood check", "How are you today?", StoryBackground.Mint, StoryFont.Modern,
            listOf({ id, _ -> SliderSticker(id, X, STICKER_Y, 1.0, 0.0, "Mood", "😍", ResultsVisibility.Everyone) }),
            emptyList(),
        ),
        TextTemplate(
            "countdown", "Countdown", "Coming soon", StoryBackground.Night, StoryFont.Strong,
            listOf({ id, now -> CountdownSticker(id, X, STICKER_Y, 1.0, 0.0, "The big day", now + WEEK_MS) }),
            listOf(emoji("🎉", 0.5, 0.18, 1.4)),
        ),
        TextTemplate("announcement", "Announcement", "Big news!", StoryBackground.Ember, StoryFont.Strong, emptyList(), listOf(emoji("📣", 0.5, 0.2, 1.6))),
        TextTemplate("gratitude", "Grateful for", "Today I'm grateful for…", StoryBackground.Forest, StoryFont.Classic, emptyList(), listOf(emoji("🌱", 0.5, 0.78, 1.4))),
        TextTemplate("quote", "Quote", "“Write something worth remembering.”", StoryBackground.Slate, StoryFont.Typewriter, emptyList(), emptyList()),
    )

    /** The templates to offer as buttons: all but the plain one (which is what a new slide already is). */
    val OFFERED: List<TextTemplate> = ALL.filter { it.id != "plain" }

    /** A template's content with fresh sticker ids, each sticker checked by the same parser a viewer uses (as the web does). */
    fun apply(t: TextTemplate, newId: () -> String, nowMs: Long): AppliedTemplate {
        val made = t.stickers.map { it(newId(), nowMs) }
        val checked = StoryStickerParser.parse(JsonArray(made.map(StoryStickerParser::toJson)))
        return AppliedTemplate(t.text, t.background, t.font, checked, t.overlays.map { it.copy() })
    }
}
