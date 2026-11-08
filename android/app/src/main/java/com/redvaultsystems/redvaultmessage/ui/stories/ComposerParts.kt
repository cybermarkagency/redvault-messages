// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import android.app.DatePickerDialog
import android.app.TimePickerDialog
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.BoxWithConstraintsScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.data.DraftSlide
import com.redvaultsystems.redvaultmessage.data.PreparedStoryMedia
import com.redvaultsystems.redvaultmessage.data.SlideMode
import com.redvaultsystems.redvaultmessage.data.StoryFriend
import com.redvaultsystems.redvaultmessage.data.model.StickerLimits
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryCaption
import com.redvaultsystems.redvaultmessage.data.model.StoryColor
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryLimits
import com.redvaultsystems.redvaultmessage.data.model.StoryOverlay
import com.redvaultsystems.redvaultmessage.data.model.StorySticker
import com.redvaultsystems.redvaultmessage.data.model.StoryStickerParser
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.security.SecureRandom
import java.util.Calendar
import kotlin.math.roundToInt

/** One slide being made. */
internal data class SlideUi(
    val key: Int,
    val mode: SlideMode = SlideMode.Text,
    val text: String = "",
    val background: StoryBackground = StoryBackground.Sunset,
    val font: StoryFont = StoryFont.Classic,
    val media: PreparedStoryMedia? = null,
    val thumbnail: ImageBitmap? = null,
    val overlays: List<StoryOverlay> = emptyList(),
    val stickers: List<StorySticker> = emptyList(),
    val captions: List<StoryCaption> = emptyList(),
) {
    /** Whether there is anything on it worth keeping. */
    val hasContent: Boolean get() = media != null || text.isNotBlank() || overlays.isNotEmpty() || stickers.isNotEmpty()

    fun toDraft() = DraftSlide(mode, text, background, font, overlays, stickers, captions, media)
}

/** What is selected on the slide being edited. */
internal sealed interface Selected {
    data class Overlay(val index: Int) : Selected
    data class Sticker(val id: String) : Selected
}

/** A fresh sticker id: 12 characters of a-z0-9 (the server only ever sees this id). */
internal fun newStickerId(): String {
    val alphabet = "abcdefghijklmnopqrstuvwxyz0123456789"
    val random = SecureRandom()
    return (0 until 12).map { alphabet[random.nextInt(alphabet.length)] }.joinToString("")
}

private val InertEnv = { friends: Map<String, StoryFriend> ->
    StickerEnv(friends, isOwner = true, interactive = false, mine = emptyMap(), totals = emptyMap(), busy = emptySet(), onAnswer = { _, _ -> }, onAskQuestion = {}, onOpenLink = {})
}

/**
 * The slide as it will look, with its text, emoji and stickers where they were put. Drag an item to move it, pinch to size and turn it, tap
 * to select it (and tap the picture to select nothing). Sizes are fractions of the frame's width, as in the viewer.
 */
@Composable
internal fun SlideEditor(
    slide: SlideUi,
    selected: Selected?,
    friends: Map<String, StoryFriend>,
    onSelect: (Selected?) -> Unit,
    onOverlay: (index: Int, StoryOverlay) -> Unit,
    onSticker: (StorySticker) -> Unit,
    modifier: Modifier = Modifier,
) {
    val density = LocalDensity.current
    BoxWithConstraints(
        modifier
            .aspectRatio(9f / 16f)
            .clip(RoundedCornerShape(16.dp))
            .background(Color.Black)
            .pointerInput(Unit) { detectTapGestures { onSelect(null) } },
    ) {
        val width = maxWidth
        val px = with(density) { maxWidth.toPx() }
        val py = with(density) { maxHeight.toPx() }
        fun cq(percent: Float) = with(density) { (width * percent / 100f).toSp() }
        when {
            slide.mode == SlideMode.Text -> {
                Box(Modifier.fillMaxSize().background(StoryStyle.brush(slide.background)))
                Text(
                    if (slide.text.isEmpty()) "Type your words below" else StoryStyle.shown(slide.font, slide.text),
                    color = if (slide.text.isEmpty()) StoryStyle.track else Color.White,
                    fontFamily = StoryStyle.family(slide.font),
                    fontWeight = StoryStyle.weight(slide.font),
                    fontSize = cq(7.5f),
                    lineHeight = cq(9.4f),
                    textAlign = TextAlign.Center,
                    modifier = Modifier.align(Alignment.Center).fillMaxWidth().padding(horizontal = width * 0.08f),
                )
            }
            slide.thumbnail != null -> Image(slide.thumbnail, contentDescription = "Your photo", contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize())
            slide.media?.kind == StoryKind.Video -> Text(
                "Video · ${(slide.media.durationMs ?: 0) / 1000} s",
                color = Color.White,
                modifier = Modifier.align(Alignment.Center),
            )
            else -> Text("Choose a photo or video below", color = StoryStyle.track, modifier = Modifier.align(Alignment.Center))
        }
        slide.overlays.forEachIndexed { i, o ->
            EditorItem(
                o.x, o.y, o.scale, o.rotation, selected == Selected.Overlay(i), 0.5, 4.0, px, py,
                onTouch = { onSelect(Selected.Overlay(i)) },
                onChange = { x, y, s, r -> onOverlay(i, o.copy(x = x, y = y, scale = s, rotation = r)) },
            ) { OverlayText(o, width, ::cq) }
        }
        val env = remember(friends) { InertEnv(friends) }
        for (s in slide.stickers) {
            EditorItem(
                s.x, s.y, s.scale, s.rotation, selected == Selected.Sticker(s.id), 0.6, 2.5, px, py,
                onTouch = { onSelect(Selected.Sticker(s.id)) },
                onChange = { x, y, sc, r -> onSticker(withPlacement(s, x, y, sc, r)) },
            ) { StickerView(s, width, env) }
        }
    }
}

/** A sticker with a new place, size and turn. */
internal fun withPlacement(s: StorySticker, x: Double, y: Double, scale: Double, rotation: Double): StorySticker = when (s) {
    is com.redvaultsystems.redvaultmessage.data.model.PollSticker -> s.copy(x = x, y = y, scale = scale, rotation = rotation)
    is com.redvaultsystems.redvaultmessage.data.model.QuizSticker -> s.copy(x = x, y = y, scale = scale, rotation = rotation)
    is com.redvaultsystems.redvaultmessage.data.model.QuestionSticker -> s.copy(x = x, y = y, scale = scale, rotation = rotation)
    is com.redvaultsystems.redvaultmessage.data.model.SliderSticker -> s.copy(x = x, y = y, scale = scale, rotation = rotation)
    is com.redvaultsystems.redvaultmessage.data.model.CountdownSticker -> s.copy(x = x, y = y, scale = scale, rotation = rotation)
    is com.redvaultsystems.redvaultmessage.data.model.LinkSticker -> s.copy(x = x, y = y, scale = scale, rotation = rotation)
    is com.redvaultsystems.redvaultmessage.data.model.MentionSticker -> s.copy(x = x, y = y, scale = scale, rotation = rotation)
    is com.redvaultsystems.redvaultmessage.data.model.DateSticker -> s.copy(x = x, y = y, scale = scale, rotation = rotation)
    is com.redvaultsystems.redvaultmessage.data.model.LocationSticker -> s.copy(x = x, y = y, scale = scale, rotation = rotation)
}

private class Place(val x: Double, val y: Double, val scale: Double, val rotation: Double)

@Composable
private fun BoxWithConstraintsScope.EditorItem(
    x: Double,
    y: Double,
    scale: Double,
    rotation: Double,
    selected: Boolean,
    minScale: Double,
    maxScale: Double,
    px: Float,
    py: Float,
    onTouch: () -> Unit,
    onChange: (x: Double, y: Double, scale: Double, rotation: Double) -> Unit,
    content: @Composable () -> Unit,
) {
    var size by remember { mutableStateOf(IntSize.Zero) }
    val current by rememberUpdatedState(Place(x, y, scale, rotation))
    Box(
        Modifier
            .align(Alignment.TopStart)
            .offset { IntOffset((x * px - size.width / 2f).roundToInt(), (y * py - size.height / 2f).roundToInt()) }
            .onSizeChanged { size = it }
            .pointerInput(Unit) { detectTapGestures { onTouch() } }
            .pointerInput(Unit) {
                detectTransformGestures { _, pan, zoom, turn ->
                    onTouch()
                    val c = current
                    onChange(
                        (c.x + pan.x / px).coerceIn(0.0, 1.0),
                        (c.y + pan.y / py).coerceIn(0.0, 1.0),
                        (c.scale * zoom).coerceIn(minScale, maxScale),
                        (((c.rotation + turn) % 360) + 360) % 360,
                    )
                }
            }
            .graphicsLayer { rotationZ = rotation.toFloat(); scaleX = scale.toFloat(); scaleY = scale.toFloat() }
            .then(if (selected) Modifier.border(2.dp, Color.White, RoundedCornerShape(8.dp)) else Modifier),
    ) { content() }
}

// --- Dialogs --------------------------------------------------------------------------------------------------------

@Composable
internal fun StyledTextField(value: String, onValue: (String) -> Unit, placeholder: String, modifier: Modifier = Modifier, singleLine: Boolean = true) {
    val colors = RedvaultTheme.colors
    TextField(
        value = value,
        onValueChange = onValue,
        placeholder = { Text(placeholder, color = colors.placeholder) },
        singleLine = singleLine,
        colors = TextFieldDefaults.colors(focusedContainerColor = colors.surface2, unfocusedContainerColor = colors.surface2, focusedTextColor = colors.text, unfocusedTextColor = colors.text),
        modifier = modifier.fillMaxWidth(),
    )
}

/** Words on the story: what they say, their color and look, and whether they sit in a box. */
@Composable
internal fun OverlayDialog(initial: StoryOverlay?, onDone: (StoryOverlay) -> Unit, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    var text by remember { mutableStateOf(initial?.text.orEmpty()) }
    var color by remember { mutableStateOf(initial?.color ?: StoryColor.White) }
    var font by remember { mutableStateOf(initial?.font ?: StoryFont.Classic) }
    var boxed by remember { mutableStateOf(initial?.boxed ?: false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface,
        title = { Text(if (initial == null) "Add words" else "Edit words", color = colors.text) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.verticalScroll(rememberScrollState())) {
                StyledTextField(text, { if (it.length <= StoryLimits.MAX_OVERLAY_TEXT) text = it }, "Type something")
                // Two short rows: ten circles in one would run off the edge of the dialog.
                for (rowColors in StoryColor.entries.chunked(5)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                        for (c in rowColors) {
                            Box(
                                Modifier
                                    .size(34.dp)
                                    .clip(CircleShape)
                                    .background(StoryStyle.color(c))
                                    .border(if (c == color) 3.dp else 1.dp, if (c == color) colors.accent else colors.border, CircleShape)
                                    .clickable { color = c }
                                    .semantics { contentDescription = c.wire },
                            )
                        }
                    }
                }
                ChipRow(options = StoryFont.entries.toList(), selected = font, label = { StoryStyle.label(it) }, onSelect = { font = it }, edge = 0.dp)
                Row(Modifier.clickable { boxed = !boxed }, verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = boxed, onCheckedChange = null, colors = CheckboxDefaults.colors(checkedColor = colors.accent, checkmarkColor = colors.accentContrast, uncheckedColor = colors.text3))
                    Text("Put it in a box", color = colors.text2, modifier = Modifier.padding(start = 8.dp))
                }
            }
        },
        confirmButton = {
            TextButton(
                enabled = text.isNotBlank(),
                onClick = {
                    onDone(
                        initial?.copy(text = text.trim(), color = color, font = font, boxed = boxed)
                            ?: StoryOverlay(false, text.trim(), 0.5, 0.5, 1.0, 0.0, color, boxed, font, null),
                    )
                },
            ) { Text("Done", color = colors.accent) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel", color = colors.text2) } },
    )
}

internal enum class StickerKind(val label: String, val interactive: Boolean) {
    Poll("Poll", true), Quiz("Quiz", true), Question("Question", true), Slider("Emoji slider", true),
    Countdown("Countdown", false), Link("Link", false), Mention("Mention", false), Date("Date or time", false), Place("Place", false),
}

private val DEADLINES = listOf(null to "None", 3_600_000L to "1 hour", 6 * 3_600_000L to "6 hours", 24 * 3_600_000L to "1 day")
private val COUNTDOWNS = listOf(3_600_000L to "1 hour", 6 * 3_600_000L to "6 hours", 24 * 3_600_000L to "1 day", 7 * 24 * 3_600_000L to "7 days")

/** What the sticker dialog has been filled in with (each kind reads only its own fields). */
internal class StickerFields(
    val question: String = "",
    val options: List<String> = listOf("", ""),
    val correct: Int = 0,
    val explanation: String = "",
    val onlyMe: Boolean = false,
    /** A poll's voting time from now, in milliseconds (none: until the story ends). */
    val deadlineMs: Long? = null,
    val anonymous: Boolean = false,
    val emoji: String = "",
    /** A countdown's length from now, in milliseconds. */
    val durationMs: Long = 0,
    val url: String = "",
    val label: String = "",
    val mention: String? = null,
    val whenMs: Long = 0,
    val dateStyle: String = "datetime",
)

/**
 * The sticker these fields make, or null when another app would drop it. The sticker is written as JSON and read back with the very rules
 * every app reads stickers with, so nothing is added that doesn't survive; it sits in the middle of the picture until it is moved.
 */
internal fun buildSticker(kind: StickerKind, f: StickerFields, now: Long, id: String): StorySticker? {
    val results = if (f.onlyMe) "author" else "everyone"
    val base: JsonObject = when (kind) {
        StickerKind.Poll -> buildJsonObject {
            put("type", "poll"); put("question", f.question); put("options", JsonArray(f.options.map { JsonPrimitive(it) })); put("results", results)
            f.deadlineMs?.let { put("closesAt", now + it) }
        }
        StickerKind.Quiz -> buildJsonObject {
            put("type", "quiz"); put("question", f.question); put("options", JsonArray(f.options.map { JsonPrimitive(it) })); put("correct", f.correct)
            put("results", results); if (f.explanation.isNotBlank()) put("explanation", f.explanation)
        }
        StickerKind.Question -> buildJsonObject { put("type", "question"); put("prompt", f.question); put("anonymous", f.anonymous) }
        StickerKind.Slider -> buildJsonObject { put("type", "slider"); put("prompt", f.question); put("emoji", f.emoji); put("results", results) }
        StickerKind.Countdown -> buildJsonObject { put("type", "countdown"); put("title", f.question); put("endsAt", now + f.durationMs) }
        StickerKind.Link -> buildJsonObject { put("type", "link"); put("url", f.url.trim()); put("label", f.label) }
        StickerKind.Mention -> buildJsonObject { put("type", "mention"); put("userId", f.mention.orEmpty()) }
        StickerKind.Date -> buildJsonObject { put("type", "date"); put("at", f.whenMs); put("style", f.dateStyle) }
        StickerKind.Place -> buildJsonObject { put("type", "location"); put("label", f.label) }
    }
    val json = JsonObject(base + mapOf("id" to JsonPrimitive(id), "x" to JsonPrimitive(0.5), "y" to JsonPrimitive(0.5), "scale" to JsonPrimitive(1.0), "rotation" to JsonPrimitive(0.0)))
    return StoryStickerParser.parse(JsonArray(listOf(json))).firstOrNull()
}

/**
 * A sticker for the story: first its kind, then its fields. What is made is read back by the very rules every app reads stickers with, so a
 * sticker another app would drop is never added. Polls, quizzes, questions and sliders are offered only while the server takes answers.
 */
@Composable
internal fun StickerDialog(friends: List<StoryFriend>, interactive: Boolean, onDone: (StorySticker) -> Unit, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    var kind by remember { mutableStateOf<StickerKind?>(null) }
    var question by remember { mutableStateOf("") }
    var options by remember { mutableStateOf(listOf("", "")) }
    var correct by remember { mutableStateOf(0) }
    var explanation by remember { mutableStateOf("") }
    var onlyMe by remember { mutableStateOf(false) }
    var deadline by remember { mutableStateOf<Long?>(null) }
    var anonymous by remember { mutableStateOf(false) }
    var emoji by remember { mutableStateOf("🔥") }
    var duration by remember { mutableStateOf(COUNTDOWNS[1].first) }
    var url by remember { mutableStateOf("https://") }
    var label by remember { mutableStateOf("") }
    var mention by remember { mutableStateOf<String?>(null) }
    var whenMs by remember { mutableStateOf(System.currentTimeMillis() + 24 * 3_600_000L) }
    var dateStyle by remember { mutableStateOf("datetime") }
    var error by remember { mutableStateOf<String?>(null) }

    fun build(): StorySticker? = kind?.let {
        buildSticker(
            it,
            StickerFields(question, options, correct, explanation, onlyMe, deadline, anonymous, emoji, duration, url, label, mention, whenMs, dateStyle),
            System.currentTimeMillis(),
            newStickerId(),
        )
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface,
        title = { Text(kind?.label ?: "Add a sticker", color = colors.text) },
        text = {
            Column(Modifier.heightIn(max = 460.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                val k = kind
                if (k == null) {
                    for (option in StickerKind.entries) {
                        val usable = !option.interactive || interactive
                        Text(
                            option.label + if (!usable) " (answers are off)" else "",
                            color = if (usable) colors.text else colors.text3,
                            fontSize = 16.sp,
                            modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).clickable(enabled = usable) { kind = option }.padding(vertical = 12.dp, horizontal = 6.dp),
                        )
                    }
                } else {
                    when (k) {
                        StickerKind.Poll, StickerKind.Quiz -> {
                            StyledTextField(question, { if (it.length <= StickerLimits.MAX_STICKER_TEXT) question = it }, "Question")
                            options.forEachIndexed { i, o ->
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    if (k == StickerKind.Quiz) {
                                        Checkbox(checked = correct == i, onCheckedChange = { correct = i }, colors = CheckboxDefaults.colors(checkedColor = colors.accent, checkmarkColor = colors.accentContrast, uncheckedColor = colors.text3))
                                    }
                                    StyledTextField(o, { v -> if (v.length <= StickerLimits.MAX_STICKER_OPTION) options = options.mapIndexed { j, x -> if (j == i) v else x } }, "Option ${i + 1}", Modifier.weight(1f))
                                }
                            }
                            if (options.size < 4) Text("+ Add an option", color = colors.accent, modifier = Modifier.clickable { options = options + "" }.padding(6.dp))
                            if (k == StickerKind.Quiz) {
                                Text("Tick the right answer.", color = colors.text3, fontSize = 12.sp)
                                StyledTextField(explanation, { if (it.length <= StickerLimits.MAX_STICKER_TEXT) explanation = it }, "Explanation (optional)")
                            } else {
                                Text("Voting ends after", color = colors.text3, fontSize = 12.sp)
                                ChipRow(options = DEADLINES, selected = DEADLINES.first { it.first == deadline }, label = { it.second }, onSelect = { deadline = it.first }, edge = 0.dp)
                            }
                            ToggleRow("Only I see the results", onlyMe) { onlyMe = it }
                        }
                        StickerKind.Question -> {
                            StyledTextField(question, { if (it.length <= StickerLimits.MAX_STICKER_TEXT) question = it }, "Ask a question")
                            ToggleRow("Answers come without a name", anonymous) { anonymous = it }
                            if (anonymous) Text("The server can still tell who sent an answer; only you can't see.", color = colors.text3, fontSize = 12.sp)
                        }
                        StickerKind.Slider -> {
                            StyledTextField(question, { if (it.length <= StickerLimits.MAX_STICKER_TEXT) question = it }, "Prompt")
                            StyledTextField(emoji, { if (it.length <= 8) emoji = it }, "Emoji")
                            ToggleRow("Only I see the results", onlyMe) { onlyMe = it }
                        }
                        StickerKind.Countdown -> {
                            StyledTextField(question, { if (it.length <= 60) question = it }, "What is it counting to?")
                            Text("Ends in", color = colors.text3, fontSize = 12.sp)
                            ChipRow(options = COUNTDOWNS, selected = COUNTDOWNS.first { it.first == duration }, label = { it.second }, onSelect = { duration = it.first }, edge = 0.dp)
                        }
                        StickerKind.Link -> {
                            StyledTextField(url, { url = it }, "https://example.com")
                            StyledTextField(label, { if (it.length <= 40) label = it }, "Label")
                            Text("Only https links. Friends are asked before they leave the app.", color = colors.text3, fontSize = 12.sp)
                        }
                        StickerKind.Mention -> {
                            if (friends.isEmpty()) Text("You have no friends to mention yet.", color = colors.text3)
                            for (f in friends.sortedBy { it.handle.lowercase() }) {
                                Text(
                                    "@" + f.handle,
                                    color = if (mention == f.userId) colors.accent else colors.text,
                                    fontWeight = if (mention == f.userId) FontWeight.Bold else FontWeight.Normal,
                                    modifier = Modifier.fillMaxWidth().clickable { mention = f.userId }.padding(vertical = 10.dp, horizontal = 6.dp),
                                )
                            }
                            Text("Nobody is notified; they see it only if they can see the story.", color = colors.text3, fontSize = 12.sp)
                        }
                        StickerKind.Date -> {
                            Text(java.text.DateFormat.getDateTimeInstance(java.text.DateFormat.MEDIUM, java.text.DateFormat.SHORT).format(java.util.Date(whenMs)), color = colors.text)
                            Text(
                                "Pick date and time",
                                color = colors.accent,
                                modifier = Modifier.clickable {
                                    val cal = Calendar.getInstance().apply { timeInMillis = whenMs }
                                    DatePickerDialog(context, { _, y, m, d ->
                                        TimePickerDialog(context, { _, h, min ->
                                            whenMs = Calendar.getInstance().apply { set(y, m, d, h, min, 0); set(Calendar.MILLISECOND, 0) }.timeInMillis
                                        }, cal.get(Calendar.HOUR_OF_DAY), cal.get(Calendar.MINUTE), true).show()
                                    }, cal.get(Calendar.YEAR), cal.get(Calendar.MONTH), cal.get(Calendar.DAY_OF_MONTH)).show()
                                }.padding(6.dp),
                            )
                            ChipRow(
                                options = listOf("date" to "Date", "time" to "Time", "datetime" to "Both"),
                                selected = listOf("date" to "Date", "time" to "Time", "datetime" to "Both").first { it.first == dateStyle },
                                label = { it.second },
                                onSelect = { dateStyle = it.first },
                                edge = 0.dp,
                            )
                        }
                        StickerKind.Place -> {
                            StyledTextField(label, { if (it.length <= 60) label = it }, "Where? (typed, never read from your phone)")
                        }
                    }
                    error?.let { Text(it, color = colors.danger, fontSize = 13.sp) }
                }
            }
        },
        confirmButton = {
            if (kind != null) {
                TextButton(onClick = {
                    val sticker = build()
                    if (sticker == null) {
                        error = when (kind) {
                            StickerKind.Link -> "Check the link: it needs https:// and a real address, and a label."
                            StickerKind.Mention -> "Choose a friend."
                            else -> "Fill in every field (options up to 40 characters, text up to 120)."
                        }
                    } else {
                        onDone(sticker)
                    }
                }) { Text("Add", color = colors.accent) }
            }
        },
        dismissButton = { TextButton(onClick = { if (kind != null) kind = null else onDismiss() }) { Text(if (kind != null) "Back" else "Cancel", color = colors.text2) } },
    )
}

@Composable
private fun ToggleRow(text: String, on: Boolean, onChange: (Boolean) -> Unit) {
    val colors = RedvaultTheme.colors
    Row(Modifier.clickable { onChange(!on) }, verticalAlignment = Alignment.CenterVertically) {
        Checkbox(checked = on, onCheckedChange = null, colors = CheckboxDefaults.colors(checkedColor = colors.accent, checkmarkColor = colors.accentContrast, uncheckedColor = colors.text3))
        Text(text, color = colors.text2, modifier = Modifier.padding(start = 8.dp))
    }
}

/** Picking friends one by one for a story that goes only to them. */
@Composable
internal fun PeopleDialog(friends: List<StoryFriend>, hidden: Set<String>, chosen: Set<String>, onDone: (Set<String>) -> Unit, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    var picked by remember { mutableStateOf(chosen) }
    val shown = friends.filter { it.userId !in hidden }.sortedBy { it.handle.lowercase() }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface,
        title = { Text("Choose people", color = colors.text) },
        text = {
            Column(Modifier.heightIn(max = 420.dp).verticalScroll(rememberScrollState())) {
                if (shown.isEmpty()) Text("You have no friends to choose from.", color = colors.text3)
                for (f in shown) {
                    val on = f.userId in picked
                    Row(
                        Modifier.fillMaxWidth().clickable { picked = if (on) picked - f.userId else picked + f.userId }.padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Checkbox(checked = on, onCheckedChange = null, colors = CheckboxDefaults.colors(checkedColor = colors.accent, checkmarkColor = colors.accentContrast, uncheckedColor = colors.text3))
                        Text(f.handle, color = colors.text, modifier = Modifier.padding(start = 8.dp))
                    }
                }
                Text("Friends you hide stories from are not listed.", color = colors.text3, fontSize = 12.sp, modifier = Modifier.padding(top = 8.dp))
            }
        },
        confirmButton = { TextButton(onClick = { onDone(picked) }) { Text("Done", color = colors.accent) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel", color = colors.text2) } },
    )
}
