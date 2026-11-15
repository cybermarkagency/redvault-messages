// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.toMutableStateList
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.redvaultsystems.redvaultmessage.data.model.StoryCaption
import com.redvaultsystems.redvaultmessage.data.model.StoryLimits
import com.redvaultsystems.redvaultmessage.data.model.StoryMetaParser
import com.redvaultsystems.redvaultmessage.data.model.jsTrim
import com.redvaultsystems.redvaultmessage.studio.StoryCaptions
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/** What a line of subtitles is while it is being typed: three texts, read only when Done is pressed. */
private class CaptionRow(val start: String, val end: String, val text: String)

private fun toRows(captions: List<StoryCaption>) = captions.map { CaptionRow(StoryCaptions.formatClock(it.startMs), StoryCaptions.formatClock(it.endMs), it.text) }

/** The most an .srt file may be before it is not read (a subtitle file is a few kilobytes). */
private const val MAX_SRT_BYTES = 512 * 1024

/**
 * Subtitles for a video or voice story, typed by the author or read from an .srt file; nothing is transcribed for you (speech recognition would
 * mean sending the recording to a service). Lines are checked as the web checks them: a start, an end after it and some text.
 */
@Composable
internal fun CaptionEditorDialog(captions: List<StoryCaption>, durationMs: Long, onChange: (List<StoryCaption>) -> Unit, onClose: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val rows = remember { toRows(captions).toMutableStateList() }
    var lines by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }

    fun apply(): Boolean {
        val parsed = ArrayList<kotlinx.serialization.json.JsonElement>()
        for (r in rows) {
            val start = StoryCaptions.parseClock(r.start)
            val end = StoryCaptions.parseClock(r.end)
            if (start == null || end == null || jsTrim(r.text).isEmpty()) {
                error = "Each line needs a start time, an end time and some text (like 0:03.5)."
                return false
            }
            if (end <= start) {
                error = "A line has to end after it starts."
                return false
            }
            if (start < durationMs) parsed.add(buildJsonObject { put("startMs", start); put("endMs", minOf(end, durationMs)); put("text", r.text) })
        }
        error = null
        onChange(StoryMetaParser.parseCaptions(JsonArray(parsed)))
        return true
    }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val text = withContext(Dispatchers.IO) {
                runCatching {
                    context.contentResolver.openInputStream(uri)?.use { input ->
                        val bytes = input.readNBytes(MAX_SRT_BYTES + 1)
                        if (bytes.size > MAX_SRT_BYTES) "" else String(bytes, Charsets.UTF_8)
                    }.orEmpty()
                }.getOrDefault("")
            }
            val cues = StoryCaptions.parseSrt(text).filter { it.startMs < durationMs }
            if (cues.isEmpty()) {
                error = "No captions could be read from that file."
            } else {
                rows.clear()
                rows.addAll(toRows(cues))
                error = null
            }
        }
    }

    Dialog(onDismissRequest = onClose, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Column(
            Modifier.padding(horizontal = 12.dp).fillMaxWidth().background(colors.surface, RoundedCornerShape(20.dp)).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text("Subtitles", color = colors.text, fontSize = 20.sp)
            Text(
                "Lines shown over the recording at the times you choose. Up to ${StoryLimits.MAX_CAPTIONS} lines of ${StoryLimits.MAX_CAPTION_TEXT} characters. Times like 0:03.5.",
                color = colors.text2,
                fontSize = 13.sp,
            )
            Column(Modifier.heightIn(max = 300.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                rows.forEachIndexed { i, r ->
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                        StyledTextField(r.start, { v -> rows[i] = CaptionRow(v, r.end, r.text) }, "Start", Modifier.width(70.dp).semantics { contentDescription = "Line ${i + 1} starts at" })
                        StyledTextField(r.end, { v -> rows[i] = CaptionRow(r.start, v, r.text) }, "End", Modifier.width(70.dp).semantics { contentDescription = "Line ${i + 1} ends at" })
                        StyledTextField(r.text, { v -> if (v.length <= StoryLimits.MAX_CAPTION_TEXT) rows[i] = CaptionRow(r.start, r.end, v) }, "Words", Modifier.weight(1f).semantics { contentDescription = "Line ${i + 1} text" })
                        TextButton(onClick = { rows.removeAt(i) }, modifier = Modifier.semantics { contentDescription = "Remove line ${i + 1}" }) { Text("×", color = colors.danger, fontSize = 20.sp) }
                    }
                }
                if (rows.isEmpty()) Text("No lines yet.", color = colors.text3, fontSize = 13.sp)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PillButton(
                    "Add a line",
                    { rows.add(CaptionRow(StoryCaptions.formatClock(0L), StoryCaptions.formatClock(minOf(2000L, durationMs)), "")) },
                    Modifier.weight(1f), PillStyle.Secondary, enabled = rows.size < StoryLimits.MAX_CAPTIONS, height = 44.dp,
                )
                PillButton("Import .srt", { picker.launch(arrayOf("*/*")) }, Modifier.weight(1f), PillStyle.Secondary, height = 44.dp)
            }
            Text("Or type lines and spread them evenly", color = colors.text2, fontSize = 13.sp)
            StyledTextField(lines, { lines = it }, "One line per subtitle", singleLine = false)
            PillButton(
                "Spread these lines",
                {
                    rows.clear()
                    rows.addAll(toRows(StoryCaptions.evenCaptions(lines.split("\n"), durationMs)))
                    lines = ""
                    error = null
                },
                style = PillStyle.Secondary,
                enabled = lines.isNotBlank(),
                height = 44.dp,
            )
            error?.let { Text(it, color = colors.danger, fontSize = 13.sp) }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                TextButton(onClick = onClose) { Text("Cancel", color = colors.text2) }
                TextButton(onClick = {
                    if (rows.isEmpty()) {
                        onChange(emptyList())
                        onClose()
                    } else if (apply()) {
                        onClose()
                    }
                }) { Text("Done", color = colors.accent) }
            }
        }
    }
}
