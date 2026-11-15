// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.stories

import android.Manifest
import android.media.MediaPlayer
import android.media.audiofx.Visualizer
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.rule.GrantPermissionRule
import com.redvaultsystems.redvaultmessage.data.model.StoryBackground
import com.redvaultsystems.redvaultmessage.data.model.StoryFont
import com.redvaultsystems.redvaultmessage.data.model.StoryKind
import com.redvaultsystems.redvaultmessage.data.model.StoryMeta
import com.redvaultsystems.redvaultmessage.studio.AudioDsp
import com.redvaultsystems.redvaultmessage.studio.Mono
import com.redvaultsystems.redvaultmessage.studio.VoiceFilter
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.sin

/**
 * A voice story played by the viewer's own frame on a device, looking at what the audio system is given: the clock moves, and the sound of a
 * known tone reaches the audio pipeline (read with Android's Visualizer on the player's own audio session). This shows the recording is
 * decoded and played, not that it comes out of a speaker (the Visualizer reads the sound before the volume is applied, so it cannot show
 * that a muted story is silent: that was read from the audio mixer instead, see docs/stories-advanced).
 */
@RunWith(AndroidJUnit4::class)
class VoiceSoundDeviceTest {
    @get:Rule val rule = createComposeRule()
    @get:Rule val permissions: GrantPermissionRule = GrantPermissionRule.grant(Manifest.permission.RECORD_AUDIO, Manifest.permission.MODIFY_AUDIO_SETTINGS)

    private fun toneFile(ms: Int, amplitude: Double): File {
        val rate = 22_050.0
        val samples = FloatArray((rate * ms / 1000).toInt()) { (amplitude * sin(2 * PI * 440.0 * it / rate)).toFloat() }
        val wav = AudioDsp.finishVoice(Mono(samples, rate), 0.0, ms.toDouble(), VoiceFilter.None).wav
        return File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "tone-test.wav").apply { writeBytes(wav) }
    }

    /** Plays [file] in the viewer's frame; returns the loudest the Visualizer saw in a few seconds (0 to 128), the clock reached and the length. */
    private fun play(file: File, listenMs: Long): Triple<Int, Long, Long> {
        var player: MediaPlayer? = null
        val position = java.util.concurrent.atomic.AtomicLong(0)
        val duration = java.util.concurrent.atomic.AtomicLong(0)
        val meta = StoryMeta(StoryKind.Audio, "Tone", StoryBackground.Night, StoryFont.Classic, emptyList(), null, emptyList(), false)
        rule.setContent {
            RedvaultTheme {
                StoryFrame(
                    meta = meta, photo = null, video = null, paused = false, caption = null, modifier = Modifier,
                    muted = false, audio = file,
                    onVideoPosition = { p, d -> position.set(p); duration.set(d) },
                    onPlayer = { player = it },
                )
            }
        }
        rule.waitUntil(10_000) { player != null && duration.get() > 0 }
        var visualizer: Visualizer? = null
        var peak = 0
        try {
            visualizer = Visualizer(player!!.audioSessionId).apply {
                captureSize = Visualizer.getCaptureSizeRange()[1]
                enabled = true
            }
            val bytes = ByteArray(visualizer.captureSize)
            val until = System.currentTimeMillis() + listenMs
            while (System.currentTimeMillis() < until) {
                Thread.sleep(50)
                if (visualizer.getWaveForm(bytes) == Visualizer.SUCCESS) peak = maxOf(peak, bytes.maxOf { abs((it.toInt() and 0xff) - 128) })
            }
        } finally {
            visualizer?.release()
        }
        // The clock reports every tenth of a second: give it time to have moved.
        rule.waitUntil(5_000) { position.get() > 300 }
        return Triple(peak, position.get(), duration.get())
    }

    @Test
    fun aPlayingVoiceStoryMovesItsClockAndPutsItsSoundIntoTheAudioPipeline() {
        val (peak, position, duration) = play(toneFile(4_000, 0.5), 1_500)
        assertEquals("the length of the recording (a little over four seconds with its fade)", 4_000.0, duration.toDouble(), 300.0)
        assertTrue("the sound of the tone reached the audio pipeline (peak $peak of 128)", peak >= 40)
        assertTrue("the clock moved ($position ms)", position > 300)
    }

    @Test
    fun aSilentRecordingPutsNoSoundIntoThePipelineSoTheTestAboveCanTellTheDifference() {
        val (peak, position, _) = play(toneFile(4_000, 0.0), 1_500)
        assertTrue("silence stays quiet (peak $peak of 128)", peak <= 4)
        assertTrue("and still plays through ($position ms)", position > 300)
    }
}
