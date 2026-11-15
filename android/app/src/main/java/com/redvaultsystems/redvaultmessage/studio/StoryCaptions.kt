// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.studio

import com.redvaultsystems.redvaultmessage.data.model.StoryCaption
import com.redvaultsystems.redvaultmessage.data.model.StoryLimits
import com.redvaultsystems.redvaultmessage.data.model.StoryMetaParser
import com.redvaultsystems.redvaultmessage.data.model.isJsSpace
import com.redvaultsystems.redvaultmessage.data.model.jsRound
import com.redvaultsystems.redvaultmessage.data.model.jsTrim
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.math.BigDecimal
import java.math.RoundingMode

/**
 * Subtitles for a video or voice story, typed by the author or read from an .srt file (port of ui/src/lib/studio/captions.ts, pinned to
 * what it makes by studio_vectors.json). Nothing is transcribed for you: speech recognition would mean sending the recording to a service.
 */
object StoryCaptions {
    private val CLOCK_PART = Regex("^[0-9]+(\\.[0-9]+)?\\z")
    private val NEWLINES = Regex("\n{2,}")
    private val TAGS = Regex("<[^>]*>")
    private val BRACES = Regex("\\{[^}]*\\}")

    /** "1:23.4" for 83,400 ms. Seconds are written to one place the way JavaScript's toFixed does it (from the exact value), so 59.95 s is "0:60.0" here as on the web. */
    fun formatClock(ms: Double): String {
        val total = maxOf(0L, jsRound(ms))
        val minutes = total / 60_000
        val seconds = (total % 60_000) / 1000.0
        val text = BigDecimal(seconds).setScale(1, RoundingMode.HALF_UP).toPlainString().padStart(4, '0')
        return "$minutes:$text"
    }

    fun formatClock(ms: Long): String = formatClock(ms.toDouble())

    /** Reads "83.4", "1:23.4", "0:01:23,400" or "00:01:23.4" as milliseconds; null if it isn't a time. */
    fun parseClock(text: String): Long? {
        val parts = jsTrim(text).replaceFirst(",", ".").split(":")
        if (parts.size > 3 || parts.any { !CLOCK_PART.matches(it) }) return null
        var seconds = 0.0
        for (p in parts) seconds = seconds * 60 + p.toDouble()
        val ms = jsRound(seconds * 1000)
        return if (ms in 0..StoryLimits.MAX_CAPTION_END_MS) ms else null
    }

    /** Reads SubRip (.srt) text. Cues that don't fit the limits are left out, as everywhere captions are read. */
    fun parseSrt(text: String): List<StoryCaption> {
        val cues = ArrayList<kotlinx.serialization.json.JsonElement>()
        for (block in text.replace("\r", "").split(NEWLINES)) {
            val lines = block.split("\n").filter { jsTrim(it).isNotEmpty() }
            val timing = lines.indexOfFirst { it.contains("-->") }
            if (timing < 0) continue
            val ends = lines[timing].split("-->")
            val start = parseClock(ends[0])
            val end = parseClock(firstWord(ends.getOrNull(1).orEmpty()))
            if (start == null || end == null) continue
            // Markup in SRT (<i>, {\an8}) is dropped: the viewer shows plain text only.
            val words = lines.drop(timing + 1).joinToString(" ").replace(TAGS, "").replace(BRACES, "")
            cues.add(buildJsonObject { put("startMs", start); put("endMs", end); put("text", words) })
        }
        return StoryMetaParser.parseCaptions(JsonArray(cues))
    }

    /** The text up to its first white space, after trimming (`trim().split(/\s+/)[0]`). */
    private fun firstWord(text: String): String = jsTrim(text).takeWhile { !isJsSpace(it) }

    fun toSrt(captions: List<StoryCaption>): String {
        fun stamp(ms: Long): String {
            val h = ms / 3_600_000
            val m = (ms % 3_600_000) / 60_000
            val s = (ms % 60_000) / 1000
            val milli = ms % 1000
            return String.format(java.util.Locale.ROOT, "%02d:%02d:%02d,%03d", h, m, s, milli)
        }
        return captions.mapIndexed { i, c -> "${i + 1}\n${stamp(c.startMs)} --> ${stamp(c.endMs)}\n${c.text}\n" }.joinToString("\n")
    }

    /** Spreads typed lines evenly over a recording: a quick way to caption speech you wrote down. */
    fun evenCaptions(lines: List<String>, durationMs: Long): List<StoryCaption> {
        val clean = lines.map { jsTrim(it) }.filter { it.isNotEmpty() }.take(StoryLimits.MAX_CAPTIONS)
        if (clean.isEmpty() || durationMs <= 0) return emptyList()
        val slot = durationMs / clean.size
        // The last line runs to the very end, so rounding never leaves it short.
        val cues = clean.mapIndexed { i, text ->
            buildJsonObject {
                put("startMs", i * slot)
                put("endMs", if (i == clean.lastIndex) durationMs else (i + 1) * slot)
                put("text", JsonPrimitive(text.take(StoryLimits.MAX_CAPTION_TEXT)))
            }
        }
        return StoryMetaParser.parseCaptions(JsonArray(cues))
    }
}
