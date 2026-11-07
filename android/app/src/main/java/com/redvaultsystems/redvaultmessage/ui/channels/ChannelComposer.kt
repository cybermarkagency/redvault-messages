package com.redvaultsystems.redvaultmessage.ui.channels

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.DatePickerDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TimePicker
import androidx.compose.material3.TimePickerDefaults
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.material3.rememberTimePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import android.content.Context
import kotlinx.coroutines.CoroutineScope
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPollRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelQuizRequest
import com.redvaultsystems.redvaultmessage.data.model.ChannelPosts
import com.redvaultsystems.redvaultmessage.ui.chats.EmojiPickerDialog
import com.redvaultsystems.redvaultmessage.ui.chats.VoiceRecorder
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle

internal enum class Extra { Poll, Quiz, Checklist }

private class Marker(val label: String, val description: String, val marker: String)

private val MARKERS = listOf(
    Marker("B", "Bold (*text*)", "*"),
    Marker("I", "Italic (_text_)", "_"),
    Marker("S", "Strikethrough (~text~)", "~"),
    Marker("</>", "Code (`text`)", "`"),
    Marker("||", "Spoiler (||text||)", "||"),
)

private const val VOICE_MAX_MS = 15 * 60 * 1000

/**
 * What the owner is writing, kept by the channel screen rather than the
 * composer, so a half-written post, its uploads and a voice clip being
 * recorded outlive the composer. Let go of when the screen closes.
 */
@Stable
class ComposerState internal constructor(context: Context, val scope: CoroutineScope, private val channels: ChannelSession, channelId: String) {
    val body = mutableStateOf(TextFieldValue(""))
    internal val extra = mutableStateOf<Extra?>(null)
    val question = mutableStateOf("")
    val options = mutableStateOf(listOf("", ""))
    val multiple = mutableStateOf(false)
    val correct = mutableIntStateOf(0)
    val explanation = mutableStateOf("")
    val listTitle = mutableStateOf("")
    val items = mutableStateOf(listOf(""))
    val collaborative = mutableStateOf(false)
    val publishAt = mutableStateOf<Instant?>(null)
    val formatting = mutableStateOf(false)
    val busy = mutableStateOf(false)
    val error = mutableStateOf<String?>(null)
    val recording = mutableStateOf(false)
    val uploads = UploadQueue(context, scope, channels.media, ChannelPosts.MAX_MEDIA_PER_POST) { meta -> channels.startMedia(channelId, meta) }
    val recorder = VoiceRecorder(context, VOICE_MAX_MS)

    suspend fun post(channelId: String, request: ChannelPostRequest): ChannelPostDto = channels.post(channelId, request)

    internal fun dispose() {
        recorder.cancel()
        recording.value = false
        uploads.clear()
    }
}

@Composable
fun rememberComposerState(channels: ChannelSession, channelId: String): ComposerState {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val state = remember(channelId) { ComposerState(context.applicationContext, scope, channels, channelId) }
    DisposableEffect(state) { onDispose { state.dispose() } }
    return state
}

/**
 * The owner's composer, the chat composer's look (attach, a growing field, send, and the mic while it's
 * empty) doing what the web's components/channel/Composer.tsx does: formatted text (with a toolbar for the
 * markers), and one of photos / videos / audio / files / voice clips / round videos, a poll, a quiz or a
 * checklist; posted now or scheduled. Everything but the text is in the attach menu, and what's been added
 * opens above the field. Posts show as the channel's.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ChannelComposer(channel: ChannelDto, state: ComposerState, onPosted: (post: ChannelPostDto, scheduled: Boolean) -> Unit, modifier: Modifier = Modifier) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val appLock = LocalAppLock.current
    val scope = state.scope
    var body by state.body
    var extra by state.extra
    var question by state.question
    var options by state.options
    var multiple by state.multiple
    var correct by state.correct
    var explanation by state.explanation
    var listTitle by state.listTitle
    var items by state.items
    var collaborative by state.collaborative
    var publishAt by state.publishAt
    var busy by state.busy
    var error by state.error
    var formatting by state.formatting
    var picking by remember { mutableStateOf(false) }
    var recordingRound by remember { mutableStateOf(false) }
    var attachMenu by remember { mutableStateOf(false) }
    var pickingEmoji by remember { mutableStateOf(false) }
    val uploads = state.uploads
    val hasMedia = !uploads.isEmpty

    // Voice clips: tap the mic to record, then Add or Cancel.
    val recorder = state.recorder
    var recording by state.recording
    var recordedMs by remember { mutableLongStateOf(recorder.elapsedMs()) }
    LaunchedEffect(recording) {
        while (recording) {
            recordedMs = recorder.elapsedMs()
            delay(250)
        }
    }
    fun finishVoice() {
        val length = recorder.elapsedMs()
        recording = false
        val file = recorder.stop() ?: return
        uploads.addRecording(file, "voice", "audio/mp4", length)
    }
    fun startVoice() {
        try {
            recorder.start(onLimitReached = { finishVoice() })
            recording = true
            error = null
        } catch (e: Exception) {
            error = "Couldn't start recording."
        }
    }
    val micPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) startVoice() else error = "Allow microphone access to record a voice clip."
    }
    val cameraPermissions = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
        if (granted.values.all { it }) recordingRound = true else error = "Allow camera and microphone access to record a round video."
    }

    val pickMedia = rememberLauncherForActivityResult(ActivityResultContracts.PickMultipleVisualMedia(ChannelPosts.MAX_MEDIA_PER_POST)) { uris ->
        uris.forEach { uploads.add(it) }
    }
    val pickAudio = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        uris.forEach { uploads.add(it, "audio") }
    }
    val pickFiles = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        uris.forEach { uploads.add(it, "file") }
    }

    fun format(marker: String) {
        val (text, start, end) = ChannelPosts.wrapSelection(body.text, body.selection.min, body.selection.max, marker)
        if (text.length <= ChannelPosts.MAX_POST) body = TextFieldValue(text, TextRange(start, end))
    }

    fun chooseExtra(next: Extra) {
        error = null
        extra = if (extra == next) null else next
    }

    fun reset() {
        body = TextFieldValue("")
        extra = null
        question = ""
        options = listOf("", "")
        multiple = false
        correct = 0
        explanation = ""
        listTitle = ""
        items = listOf("")
        collaborative = false
        publishAt = null
        uploads.clear()
    }

    fun submit() {
        busy = true
        error = null
        scope.launch {
            try {
                val at = publishAt?.takeIf { it.isAfter(Instant.now()) }
                val request = ChannelPostRequest(
                    body = body.text,
                    poll = when (extra) {
                        Extra.Poll, Extra.Quiz -> ChannelPollRequest(
                            question = question,
                            options = options.map { it.trim() }.filter { it.isNotEmpty() },
                            multiple = extra == Extra.Poll && multiple,
                            quiz = if (extra == Extra.Quiz) ChannelQuizRequest(correct, explanation) else null,
                        )
                        else -> null
                    },
                    checklist = if (extra == Extra.Checklist) {
                        ChannelChecklistRequest(listTitle, items.map { it.trim() }.filter { it.isNotEmpty() }, collaborative)
                    } else {
                        null
                    },
                    media = if (extra == null) uploads.ready.takeIf { it.isNotEmpty() } else null,
                    publishAt = at?.let { DateTimeFormatter.ISO_INSTANT.format(it) },
                )
                val created = state.post(channel.id, request)
                onPosted(created, at != null)
                reset()
            } catch (e: Exception) {
                error = describeError(e, "Couldn't post. Try again.")
            } finally {
                busy = false
            }
        }
    }

    val hasText = body.text.isNotBlank()
    val canPost = ChannelThread.canPost(busy, uploads.busy, recording, hasText, extra != null, hasMedia, uploads.ready.size)
    val canAttach = extra == null && !uploads.full
    val hasPanel = !uploads.isEmpty || extra != null || publishAt != null || error != null

    fun recordVoice() {
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) startVoice()
        else micPermission.launch(Manifest.permission.RECORD_AUDIO)
    }

    Column(modifier.fillMaxWidth().background(colors.bg)) {
        // Above the field, and scrolling on its own: a long poll can't push the field off the screen or under the keyboard.
        if (hasPanel) {
            Column(
                Modifier
                    .weight(1f, fill = false)
                    .heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.45f).dp)
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState())
                    .padding(start = 12.dp, end = 12.dp, top = 8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                UploadList(uploads)

                if (extra == Extra.Poll || extra == Extra.Quiz) {
                    Builder {
                        DsTextField(question, { if (it.length <= ChannelPosts.MAX_POLL_QUESTION) question = it }, if (extra == Extra.Quiz) "Quiz question" else "Poll question")
                        options.forEachIndexed { i, option ->
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                if (extra == Extra.Quiz) {
                                    Choice(selected = correct == i, description = "Option ${i + 1} is the right answer") { correct = i }
                                }
                                Box(Modifier.weight(1f)) {
                                    DsTextField(option, { v -> if (v.length <= ChannelPosts.MAX_POLL_OPTION) options = options.mapIndexed { j, o -> if (j == i) v else o } }, "Option ${i + 1}")
                                }
                                if (options.size > 2) {
                                    TextButton(onClick = {
                                        options = options.filterIndexed { j, _ -> j != i }
                                        if (correct >= i && correct > 0) correct -= 1
                                    }) { Text("Remove", color = colors.text2, fontSize = 13.sp) }
                                }
                            }
                        }
                        if (options.size < ChannelPosts.MAX_POLL_OPTIONS) LinkButton("Add an option") { options = options + "" }
                        if (extra == Extra.Poll) CheckRow("Let people pick more than one", multiple) { multiple = it }
                        if (extra == Extra.Quiz) {
                            DsTextField(explanation, { if (it.length <= ChannelPosts.MAX_EXPLANATION) explanation = it }, "Why it's right (optional, shown after answering)")
                        }
                        LinkButton("Remove this ${if (extra == Extra.Quiz) "quiz" else "poll"}") { extra = null }
                    }
                }

                if (extra == Extra.Checklist) {
                    Builder {
                        DsTextField(listTitle, { if (it.length <= ChannelPosts.MAX_CHECKLIST_TITLE) listTitle = it }, "Checklist title")
                        items.forEachIndexed { i, item ->
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                Box(Modifier.weight(1f)) {
                                    DsTextField(item, { v -> if (v.length <= ChannelPosts.MAX_CHECKLIST_ITEM) items = items.mapIndexed { j, x -> if (j == i) v else x } }, "Item ${i + 1}")
                                }
                                if (items.size > 1) {
                                    TextButton(onClick = { items = items.filterIndexed { j, _ -> j != i } }) { Text("Remove", color = colors.text2, fontSize = 13.sp) }
                                }
                            }
                        }
                        if (items.size < ChannelPosts.MAX_CHECKLIST_ITEMS) LinkButton("Add an item") { items = items + "" }
                        CheckRow("Followers can tick items too", collaborative) { collaborative = it }
                        LinkButton("Remove this checklist") { extra = null }
                    }
                }

                publishAt?.let { at ->
                    FlowRow(verticalArrangement = Arrangement.Center, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        Row(Modifier.heightIn(min = 40.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            DsIcon(R.drawable.ui_clock, colors.text2, 16.dp)
                            Text("Posts at ${whenText(at)}", color = colors.text, fontSize = 14.sp)
                        }
                        TextButton(onClick = { picking = true }) { Text("Change", color = colors.accent) }
                        TextButton(onClick = { publishAt = null }) { Text("Post now instead", color = colors.text2) }
                    }
                }

                error?.let { Notice(it) }
            }
        }

        if (formatting) {
            Row(
                Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, top = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                MARKERS.forEach { m -> ToolButton(m) { format(m.marker) } }
                Text("Links are made automatically", color = colors.text3, fontSize = 11.sp, modifier = Modifier.weight(1f).padding(start = 4.dp))
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Box {
                IconButton(onClick = { attachMenu = true }) {
                    Icon(painterResource(R.drawable.ic_attach), contentDescription = "Attach media, a poll, or more", tint = colors.text2)
                }
                DropdownMenu(expanded = attachMenu, onDismissRequest = { attachMenu = false }) {
                    AttachItem("Photo or video", R.drawable.ui_image, enabled = canAttach) {
                        attachMenu = false
                        appLock?.expectExternalReturn()
                        pickMedia.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageAndVideo))
                    }
                    AttachItem("Audio", R.drawable.ui_headphones, enabled = canAttach) {
                        attachMenu = false
                        appLock?.expectExternalReturn()
                        pickAudio.launch(arrayOf("audio/*"))
                    }
                    AttachItem("File", R.drawable.ui_file, enabled = canAttach) {
                        attachMenu = false
                        appLock?.expectExternalReturn()
                        pickFiles.launch(arrayOf("*/*"))
                    }
                    AttachItem("Voice clip", R.drawable.ui_mic, enabled = canAttach && !recording) {
                        attachMenu = false
                        recordVoice()
                    }
                    AttachItem("Round video", R.drawable.ui_video, enabled = canAttach && !recording) {
                        attachMenu = false
                        val needed = arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
                        if (needed.all { ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED }) recordingRound = true
                        else cameraPermissions.launch(needed)
                    }
                    AttachItem("Poll", R.drawable.ui_signal, on = extra == Extra.Poll, enabled = !hasMedia) {
                        attachMenu = false
                        chooseExtra(Extra.Poll)
                    }
                    AttachItem("Quiz", R.drawable.ui_star, on = extra == Extra.Quiz, enabled = !hasMedia) {
                        attachMenu = false
                        chooseExtra(Extra.Quiz)
                    }
                    AttachItem("Checklist", R.drawable.ui_note, on = extra == Extra.Checklist, enabled = !hasMedia) {
                        attachMenu = false
                        chooseExtra(Extra.Checklist)
                    }
                    AttachItem("Schedule", R.drawable.ui_clock, on = publishAt != null) {
                        attachMenu = false
                        picking = true
                    }
                    AttachItem("Formatting", R.drawable.ui_pen, on = formatting) {
                        attachMenu = false
                        formatting = !formatting
                    }
                }
            }
            if (recording) {
                Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
                    Text("● ", color = colors.danger)
                    Text(
                        "${ChannelPosts.formatDuration(recordedMs / 1000.0)} / ${ChannelPosts.formatDuration(VOICE_MAX_MS / 1000.0)}",
                        color = colors.text,
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier.weight(1f),
                    )
                    TextButton(onClick = {
                        recording = false
                        recorder.cancel()
                    }) { Text("Cancel", color = colors.text2) }
                    TextButton(onClick = ::finishVoice) { Text("Add", color = colors.accent, fontWeight = FontWeight.SemiBold) }
                }
                return@Row
            }
            OutlinedTextField(
                value = body,
                onValueChange = { if (it.text.length <= ChannelPosts.MAX_POST) body = it },
                placeholder = { Text("Write a post", maxLines = 1) },
                maxLines = 5,
                shape = RoundedCornerShape(22.dp),
                trailingIcon = {
                    IconButton(onClick = { pickingEmoji = true }) {
                        Icon(painterResource(R.drawable.ui_smile), contentDescription = "Emoji", tint = colors.text2)
                    }
                },
                modifier = Modifier.weight(1f),
            )
            if (ChannelThread.showMic(hasText, extra != null, hasMedia)) {
                IconButton(onClick = ::recordVoice) {
                    Icon(painterResource(R.drawable.ic_mic), contentDescription = "Record a voice clip", tint = colors.text2)
                }
                return@Row
            }
            IconButton(enabled = canPost, onClick = ::submit) {
                Icon(
                    painterResource(R.drawable.ic_send),
                    contentDescription = if (busy) "Posting" else if (publishAt != null) "Schedule post" else "Post",
                    tint = if (canPost) colors.accent else colors.text3,
                )
            }
        }

        // What the old composer's footer said, while it applies.
        if (body.text.isNotEmpty() || uploads.busy) {
            Text(
                "${body.text.length}/${ChannelPosts.MAX_POST} · posted as ${channel.name}" + if (uploads.busy) " · uploading…" else "",
                color = colors.text3,
                fontSize = 12.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(start = 16.dp, end = 16.dp, bottom = 6.dp),
            )
        }
    }

    if (pickingEmoji) {
        EmojiPickerDialog(
            onPick = { emoji ->
                pickingEmoji = false
                val at = body.selection
                val text = body.text.replaceRange(at.min, at.max, emoji)
                if (text.length <= ChannelPosts.MAX_POST) body = TextFieldValue(text, TextRange(at.min + emoji.length))
            },
            onDismiss = { pickingEmoji = false },
        )
    }
    if (picking) {
        ScheduleDialog(initial = publishAt, onDismiss = { picking = false }) { at ->
            picking = false
            publishAt = at
        }
    }
    if (recordingRound) {
        RoundVideoRecorder(
            onCancel = { recordingRound = false },
            onDone = { file, durationMs ->
                recordingRound = false
                uploads.addRecording(file, "round", "video/mp4", durationMs)
            },
        )
    }
}

/** The post's text field: keeps the selection, so the toolbar can wrap it. */
@Composable
fun FormattedField(value: TextFieldValue, onValueChange: (TextFieldValue) -> Unit, placeholder: String, minHeight: Dp = 100.dp) {
    val colors = RedvaultTheme.colors
    var focused by remember { mutableStateOf(false) }
    Box(
        Modifier
            .fillMaxWidth()
            .heightIn(min = minHeight)
            .background(colors.surface2, RoundedCornerShape(12.dp))
            .border(if (focused) 2.dp else 1.dp, if (focused) colors.accent else colors.border, RoundedCornerShape(12.dp))
            .padding(14.dp),
    ) {
        if (value.text.isEmpty()) Text(placeholder, color = colors.placeholder, fontSize = 15.sp, fontFamily = RedvaultFonts.sans)
        BasicTextField(
            value = value,
            onValueChange = onValueChange,
            textStyle = TextStyle(color = colors.text, fontSize = 15.sp, fontFamily = RedvaultFonts.sans),
            cursorBrush = SolidColor(colors.accent),
            modifier = Modifier.fillMaxWidth().semantics { contentDescription = placeholder }.onFocusChanged { focused = it.isFocused },
        )
    }
}

@Composable
private fun ToolButton(marker: Marker, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Box(
        Modifier.size(44.dp).clip(RoundedCornerShape(8.dp)).background(colors.surface2)
            .clickable(role = Role.Button, onClick = onClick)
            .semantics { contentDescription = marker.description },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            marker.label,
            color = colors.text,
            fontSize = 13.sp,
            fontWeight = if (marker.marker == "*") FontWeight.Bold else FontWeight.SemiBold,
            fontStyle = if (marker.marker == "_") FontStyle.Italic else FontStyle.Normal,
            textDecoration = if (marker.marker == "~") TextDecoration.LineThrough else null,
            fontFamily = if (marker.marker == "`") FontFamily.Monospace else RedvaultFonts.sans,
        )
    }
}

/** One entry in the attach menu; [on] ticks what's already chosen. */
@Composable
private fun AttachItem(label: String, icon: Int, on: Boolean = false, enabled: Boolean = true, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    DropdownMenuItem(
        text = { Text(label) },
        leadingIcon = { DsIcon(icon, if (enabled) colors.text2 else colors.text3, 20.dp) },
        trailingIcon = if (on) ({ DsIcon(R.drawable.ui_check, colors.accent, 16.dp) }) else null,
        enabled = enabled,
        onClick = onClick,
        modifier = Modifier.semantics { selected = on },
    )
}

@Composable
private fun Builder(content: @Composable () -> Unit) {
    val colors = RedvaultTheme.colors
    Column(
        Modifier.fillMaxWidth().border(1.dp, colors.border, RoundedCornerShape(14.dp)).padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) { content() }
}

@Composable
private fun Choice(selected: Boolean, description: String, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Box(
        Modifier.size(24.dp).clip(CircleShape).border(2.dp, if (selected) colors.accent else colors.text3, CircleShape)
            .clickable(role = Role.RadioButton, onClick = onClick)
            .semantics {
                contentDescription = description
                this.selected = selected
            },
        contentAlignment = Alignment.Center,
    ) {
        if (selected) Box(Modifier.size(12.dp).clip(CircleShape).background(colors.accent))
    }
}

@Composable
fun CheckRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier.clip(RoundedCornerShape(8.dp)).clickable(role = Role.Checkbox) { onChange(!checked) }.padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(
            Modifier.size(20.dp).clip(RoundedCornerShape(5.dp))
                .background(if (checked) colors.accent else androidx.compose.ui.graphics.Color.Transparent)
                .border(1.5.dp, if (checked) colors.accent else colors.text3, RoundedCornerShape(5.dp)),
            contentAlignment = Alignment.Center,
        ) {
            if (checked) DsIcon(R.drawable.ui_check, colors.accentContrast, 14.dp)
        }
        Text(label, color = colors.text, fontSize = 14.sp)
    }
}

@Composable
private fun LinkButton(text: String, onClick: () -> Unit) {
    TextButton(onClick = onClick) { Text(text, color = RedvaultTheme.colors.accent, fontWeight = FontWeight.SemiBold) }
}

fun whenText(at: Instant): String =
    DateTimeFormatter.ofLocalizedDateTime(FormatStyle.MEDIUM, FormatStyle.SHORT).format(at.atZone(ZoneId.systemDefault()))

/** Picks a day, then a time; a moment that's already past posts straight away. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ScheduleDialog(initial: Instant?, onDismiss: () -> Unit, onPicked: (Instant) -> Unit) {
    val colors = RedvaultTheme.colors
    val zone = ZoneId.systemDefault()
    val start = (initial ?: Instant.now().plusSeconds(3600)).atZone(zone)
    var day by remember { mutableStateOf<LocalDate?>(null) }
    val dateState = rememberDatePickerState(
        initialSelectedDateMillis = start.toLocalDate().atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli(),
    )
    val timeState = rememberTimePickerState(initialHour = start.hour, initialMinute = start.minute)
    val chosenDay = day
    if (chosenDay == null) {
        DatePickerDialog(
            onDismissRequest = onDismiss,
            confirmButton = {
                TextButton(onClick = {
                    val millis = dateState.selectedDateMillis ?: return@TextButton
                    day = Instant.ofEpochMilli(millis).atZone(ZoneOffset.UTC).toLocalDate()
                }) { Text("Next", color = colors.accent) }
            },
            dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel", color = colors.text2) } },
            colors = DatePickerDefaults.colors(containerColor = colors.surface),
        ) {
            DatePicker(
                state = dateState,
                colors = DatePickerDefaults.colors(
                    containerColor = colors.surface,
                    titleContentColor = colors.text2,
                    headlineContentColor = colors.text,
                    weekdayContentColor = colors.text3,
                    dayContentColor = colors.text,
                    selectedDayContainerColor = colors.accent,
                    selectedDayContentColor = colors.accentContrast,
                    todayContentColor = colors.accent,
                    todayDateBorderColor = colors.accent,
                ),
            )
        }
    } else {
        AlertDialog(
            onDismissRequest = onDismiss,
            containerColor = colors.surface,
            title = { Text("Post at", color = colors.text) },
            text = {
                TimePicker(
                    state = timeState,
                    colors = TimePickerDefaults.colors(
                        clockDialColor = colors.surface2,
                        selectorColor = colors.accent,
                        timeSelectorSelectedContainerColor = colors.accentSoft,
                        timeSelectorUnselectedContainerColor = colors.surface2,
                        timeSelectorSelectedContentColor = colors.text,
                        timeSelectorUnselectedContentColor = colors.text2,
                        clockDialSelectedContentColor = colors.accentContrast,
                        clockDialUnselectedContentColor = colors.text,
                        periodSelectorSelectedContainerColor = colors.accentSoft,
                        periodSelectorSelectedContentColor = colors.text,
                        periodSelectorUnselectedContentColor = colors.text2,
                    ),
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    onPicked(chosenDay.atTime(LocalTime.of(timeState.hour, timeState.minute)).atZone(zone).toInstant())
                }) { Text("Done", color = colors.accent) }
            },
            dismissButton = { TextButton(onClick = { day = null }) { Text("Back", color = colors.text2) } },
        )
    }
}

/** A small secondary pill for actions inside a channel card. */
@Composable
fun SmallPill(text: String, onClick: () -> Unit, enabled: Boolean = true, style: PillStyle = PillStyle.Secondary) {
    PillButton(text, onClick, style = style, enabled = enabled, height = 36.dp)
}
