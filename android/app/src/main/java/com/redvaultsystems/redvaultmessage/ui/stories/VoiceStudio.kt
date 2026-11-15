// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import android.Manifest
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.MediaPlayer
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.PreparedStoryMedia
import com.redvaultsystems.redvaultmessage.data.StoryInputException
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryCaption
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryLimits
import com.redvaultsystems.redvaultmessage.studio.AudioDsp
import com.redvaultsystems.redvaultmessage.studio.AudioFileDecoder
import com.redvaultsystems.redvaultmessage.studio.Mono
import com.redvaultsystems.redvaultmessage.studio.StoryVoiceRecorder
import com.redvaultsystems.redvaultmessage.studio.VoiceFilter
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.SectionCaps
import com.redvaultsystems.redvaultmessage.ui.components.SheetBar
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileInputStream
import kotlin.math.min

/** What the voice studio hands back: the recording as a WAV, and the title, look and subtitles that go with it. */
internal class VoiceResult(val media: PreparedStoryMedia, val title: String, val background: StoryBackground, val captions: List<StoryCaption>)

/** How the studio opens: straight into recording, into the file picker, or onto a recording already made (its own WAV). */
internal sealed interface VoiceStart {
    data object Record : VoiceStart
    data object Pick : VoiceStart
    class Edit(val media: PreparedStoryMedia, val title: String, val background: StoryBackground, val captions: List<StoryCaption>) : VoiceStart
}

private enum class Phase { Choose, Recording, Edit }

private fun clock(ms: Long) = "%d:%02d".format(ms / 60_000, (ms % 60_000) / 1000)

/**
 * Voice and audio stories, edited on this phone: record or choose a file, cut the start and end, change the voice, add a title and subtitles
 * (the web studio's AudioStudio). The microphone is only asked for when someone presses Record. What comes out is a WAV of 22 kHz mono sound
 * (the maths is [AudioDsp], pinned to the web's), and only its encrypted form is ever uploaded.
 */
@Composable
internal fun VoiceStudio(start: VoiceStart?, onDone: (VoiceResult) -> Unit, onCancel: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val recorder = remember { StoryVoiceRecorder() }
    var phase by remember { mutableStateOf(Phase.Choose) }
    var mono by remember { mutableStateOf<Mono?>(null) }
    var trimStart by remember { mutableLongStateOf(0) }
    var trimEnd by remember { mutableLongStateOf(0) }
    var filter by remember { mutableStateOf(VoiceFilter.None) }
    var title by remember { mutableStateOf(if (start is VoiceStart.Edit) start.title else "") }
    var background by remember { mutableStateOf(if (start is VoiceStart.Edit) start.background else StoryBackground.Night) }
    var captions by remember { mutableStateOf(if (start is VoiceStart.Edit) start.captions else emptyList()) }
    var captionsOpen by remember { mutableStateOf(false) }
    var elapsed by remember { mutableLongStateOf(0) }
    var level by remember { mutableFloatStateOf(0f) }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var playing by remember { mutableStateOf(false) }
    val player = remember { arrayOfNulls<MediaPlayer>(1) }

    fun stopPlaying() {
        player[0]?.let { runCatching { it.stop() }; it.release() }
        player[0] = null
        playing = false
    }
    DisposableEffect(Unit) { onDispose { recorder.cancel(); stopPlaying() } }

    val total = mono?.let { AudioDsp.durationMs(it) } ?: 0L

    fun load(sound: Mono) {
        busy = true
        error = null
        scope.launch {
            try {
                // Voices don't need CD quality, and it keeps what is held in memory small.
                val small = withContext(Dispatchers.Default) { if (sound.sampleRate > AudioDsp.STORY_AUDIO_RATE) AudioDsp.resample(sound, AudioDsp.STORY_AUDIO_RATE) else sound }
                val ms = AudioDsp.durationMs(small)
                if (ms < 300) throw StoryInputException("That recording is too short.")
                mono = small
                trimStart = 0
                trimEnd = min(ms, StoryLimits.MAX_AUDIO_MS)
                phase = Phase.Edit
            } catch (e: CancellationException) {
                throw e
            } catch (e: StoryInputException) {
                error = e.message
                phase = Phase.Choose
            } finally {
                busy = false
            }
        }
    }

    fun finishRecording() {
        val sound = recorder.stop()
        if (sound == null) {
            error = "Nothing was recorded. Try again, or choose a file."
            phase = Phase.Choose
        } else {
            load(sound)
        }
    }

    fun beginRecording() {
        error = null
        if (!recorder.start(StoryLimits.MAX_AUDIO_MS, onLimit = ::finishRecording)) {
            error = "The microphone couldn't be opened. Choose an audio file instead."
            return
        }
        elapsed = 0
        phase = Phase.Recording
    }

    val microphone = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) beginRecording() else error = "Allow the microphone in the phone's settings to record, or choose an audio file."
    }
    fun record() {
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) beginRecording() else microphone.launch(Manifest.permission.RECORD_AUDIO)
    }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        busy = true
        error = null
        scope.launch {
            try {
                val sound = AudioFileDecoder.decode(context, uri)
                busy = false
                load(sound)
            } catch (e: CancellationException) {
                throw e
            } catch (e: StoryInputException) {
                error = e.message
                busy = false
            } catch (e: Exception) {
                error = "That audio couldn't be opened."
                busy = false
            }
        }
    }

    LaunchedEffect(Unit) {
        when (start) {
            VoiceStart.Record -> record()
            VoiceStart.Pick -> picker.launch(arrayOf("audio/*"))
            is VoiceStart.Edit -> {
                val sound = withContext(Dispatchers.IO) { runCatching { AudioDsp.decodeWav(start.media.open().use { it.readBytes() }) }.getOrNull() }
                if (sound == null) error = "That recording couldn't be opened again." else load(sound)
            }
            null -> {}
        }
    }
    LaunchedEffect(phase) {
        while (phase == Phase.Recording) {
            elapsed = recorder.elapsedMs
            level = recorder.level
            delay(80)
        }
    }

    BackHandler {
        when {
            busy -> {}
            phase == Phase.Recording -> { recorder.cancel(); phase = Phase.Choose }
            else -> onCancel()
        }
    }

    fun listen() {
        if (playing) return stopPlaying()
        val sound = mono ?: return
        scope.launch {
            try {
                val done = withContext(Dispatchers.Default) { AudioDsp.finishVoice(sound, trimStart.toDouble(), trimEnd.toDouble(), filter) }
                val file = withContext(Dispatchers.IO) { File(voiceDir(context), "preview.wav").also { it.parentFile?.mkdirs(); it.writeBytes(done.wav) } }
                val p = MediaPlayer()
                p.setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                p.setDataSource(file.absolutePath)
                p.setOnCompletionListener { stopPlaying() }
                p.setOnErrorListener { _, _, _ -> stopPlaying(); true }
                p.setOnPreparedListener { it.start() }
                player[0] = p
                playing = true
                p.prepareAsync()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                stopPlaying()
                error = "That couldn't be played."
            }
        }
    }

    fun finish() {
        val sound = mono ?: return
        stopPlaying()
        busy = true
        error = null
        scope.launch {
            try {
                val done = withContext(Dispatchers.Default) { AudioDsp.finishVoice(sound, trimStart.toDouble(), trimEnd.toDouble(), filter) }
                if (done.wav.size > StoryLimits.MAX_AUDIO_BYTES) throw StoryInputException("That audio is too large for a story (at most 24 MB).")
                val file = withContext(Dispatchers.IO) { File(voiceDir(context), "voice-${System.nanoTime()}.wav").also { it.parentFile?.mkdirs(); it.writeBytes(done.wav) } }
                val clipped = captions.filter { it.startMs < done.durationMs }.map { StoryCaption(it.startMs, min(it.endMs, done.durationMs), it.text) }
                val media = PreparedStoryMedia(
                    StoryKind.Audio, "audio/wav", 1, 1, min(done.durationMs, StoryLimits.MAX_AUDIO_MS), file.length(), done.waveform,
                ) { FileInputStream(file) }
                onDone(VoiceResult(media, title.trim().take(120), background, clipped))
            } catch (e: CancellationException) {
                throw e
            } catch (e: StoryInputException) {
                error = e.message
                busy = false
            } catch (e: Exception) {
                error = "That audio couldn't be prepared."
                busy = false
            }
        }
    }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        SheetBar("Voice and audio", onCancel = { if (!busy) { recorder.cancel(); onCancel() } }, cancelText = "Cancel")
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            when (phase) {
                Phase.Choose -> {
                    PillButton("Record your voice", ::record, enabled = !busy, modifier = Modifier.fillMaxWidth())
                    PillButton("Choose an audio file", { picker.launch(arrayOf("audio/*")) }, style = PillStyle.Secondary, enabled = !busy, modifier = Modifier.fillMaxWidth())
                    if (busy) Text("Getting it ready…", color = colors.text2)
                    Text("Edited here on your phone. Only the encrypted result is ever uploaded. Use audio you have the right to share.", color = colors.text3, fontSize = 12.sp)
                }
                Phase.Recording -> {
                    Box(
                        Modifier.fillMaxWidth().height(14.dp).clip(RoundedCornerShape(7.dp)).background(colors.surface2).semantics { contentDescription = "Microphone level" },
                    ) {
                        Box(Modifier.fillMaxWidth(fraction = level.coerceIn(0.02f, 1f)).height(14.dp).background(colors.accent))
                    }
                    Text("${clock(elapsed)} / ${clock(StoryLimits.MAX_AUDIO_MS)}", color = colors.text, fontSize = 28.sp, fontWeight = FontWeight.SemiBold)
                    PillButton("Stop", ::finishRecording, modifier = Modifier.fillMaxWidth())
                }
                Phase.Edit -> mono?.let { sound ->
                    val bars = remember(sound) { AudioDsp.waveformBars(sound.samples, 48) }
                    val from = if (total > 0) trimStart.toFloat() / total * bars.size else 0f
                    val to = if (total > 0) trimEnd.toFloat() / total * bars.size else 0f
                    Canvas(Modifier.fillMaxWidth().height(110.dp).semantics { contentDescription = "Recording, ${clock(total)} long" }) {
                        val slot = size.width / bars.size
                        val barWidth = slot * 0.62f
                        bars.forEachIndexed { i, v ->
                            val h = size.height * (8 + v * 0.9f) / 100f
                            drawRoundRect(
                                color = if (i >= from - 0.5f && i < to) colors.accent else colors.surface2,
                                topLeft = Offset(i * slot + (slot - barWidth) / 2, (size.height - h) / 2),
                                size = Size(barWidth, h),
                                cornerRadius = CornerRadius(barWidth / 2),
                            )
                        }
                    }
                    Text("Keeping ${clock(trimStart)} to ${clock(trimEnd)} of ${clock(total)}", color = colors.text2, fontSize = 13.sp)
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("Start", color = colors.text2, fontSize = 13.sp, modifier = Modifier.width(44.dp))
                        Slider(
                            value = trimStart.toFloat(),
                            onValueChange = {
                                trimStart = (it.toLong() / 100) * 100
                                if (trimEnd < trimStart + 500) trimEnd = min(total, trimStart + 500)
                                stopPlaying()
                            },
                            valueRange = 0f..maxOf(0L, total - 500).toFloat().coerceAtLeast(1f),
                            colors = SliderDefaults.colors(thumbColor = colors.accent, activeTrackColor = colors.accent, inactiveTrackColor = colors.border),
                            modifier = Modifier.weight(1f).semantics { contentDescription = "Start of the part to keep" },
                        )
                    }
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("End", color = colors.text2, fontSize = 13.sp, modifier = Modifier.width(44.dp))
                        Slider(
                            value = trimEnd.toFloat(),
                            onValueChange = {
                                trimEnd = (it.toLong() / 100) * 100
                                if (trimStart > trimEnd - 500) trimStart = maxOf(0, trimEnd - 500)
                                stopPlaying()
                            },
                            valueRange = 500f..maxOf(total.toFloat(), 501f),
                            colors = SliderDefaults.colors(thumbColor = colors.accent, activeTrackColor = colors.accent, inactiveTrackColor = colors.border),
                            modifier = Modifier.weight(1f).semantics { contentDescription = "End of the part to keep" },
                        )
                    }
                    SectionCaps("Voice", edge = 0.dp)
                    ChipRow(options = VoiceFilter.entries.toList(), selected = filter, label = { it.label }, onSelect = { filter = it; stopPlaying() }, edge = 0.dp)
                    PillButton(if (playing) "Stop listening" else "Listen", ::listen, style = PillStyle.Secondary, modifier = Modifier.fillMaxWidth())
                    StyledTextField(title, { if (it.length <= 120) title = it }, "Add a title (optional)")
                    SectionCaps("Background", edge = 0.dp)
                    ChipRow(options = StoryBackground.entries.toList(), selected = background, label = { StoryStyle.label(it) }, onSelect = { background = it }, edge = 0.dp)
                    PillButton(
                        if (captions.isEmpty()) "Subtitles" else "Subtitles (${captions.size})",
                        { captionsOpen = true },
                        style = PillStyle.Secondary,
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
            }
            error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
        }
        if (phase == Phase.Edit) {
            PillButton(if (busy) "Preparing…" else "Use this", ::finish, enabled = !busy, busy = busy, modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 12.dp))
        }
    }
    if (captionsOpen) {
        CaptionEditorDialog(captions, durationMs = maxOf(1000L, trimEnd - trimStart), onChange = { captions = it }, onClose = { captionsOpen = false })
    }
}

/** Where recordings and previews are kept until the session ends (the cache, emptied with the rest of the story cache). */
internal fun voiceDir(context: android.content.Context) = File(context.cacheDir, "stories/voice")
