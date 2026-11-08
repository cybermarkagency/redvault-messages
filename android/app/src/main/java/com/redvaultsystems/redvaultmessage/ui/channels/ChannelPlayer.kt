// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.channels

import android.media.MediaDataSource
import android.media.MediaPlayer
import android.view.Surface
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** Only one channel video or recording plays at a time: starting one pauses the other. */
private object NowPlaying {
    private var current: ChannelPlayer? = null

    fun take(player: ChannelPlayer) {
        if (current !== player) current?.pause()
        current = player
    }

    fun drop(player: ChannelPlayer) {
        if (current === player) current = null
    }
}

/**
 * One video or recording in a channel post, played with the phone's
 * MediaPlayer from [source] (a RangeSource: bytes fetched by our own client,
 * so the player's own HTTP stack never runs). Nothing is fetched until Play.
 * [onPosition] is told where playback is about every five seconds, and when
 * it pauses or ends (0), for podcasts' "resume where you stopped".
 */
@Stable
class ChannelPlayer(
    private val source: () -> MediaDataSource?,
    private val resumeAtSeconds: Int = 0,
    private val onPosition: ((seconds: Int) -> Unit)? = null,
) {
    enum class Phase { Idle, Preparing, Ready, Failed }

    var phase by mutableStateOf(Phase.Idle)
        private set
    var playing by mutableStateOf(false)
        private set
    var positionMs by mutableLongStateOf(0L)
        private set
    var durationMs by mutableLongStateOf(0L)
        private set
    var speed by mutableFloatStateOf(1f)
        private set
    /** The video's size once known (0 for audio). */
    var videoWidth by mutableStateOf(0)
        private set
    var videoHeight by mutableStateOf(0)
        private set

    private var player: MediaPlayer? = null
    private var surface: Surface? = null
    private var playWhenReady = false
    private var lastReported = 0L

    fun toggle() = if (playing) pause() else play()

    fun play() {
        NowPlaying.take(this)
        val p = player
        when {
            p == null || phase == Phase.Failed -> {
                playWhenReady = true
                prepare()
            }
            phase != Phase.Ready -> playWhenReady = true
            else -> start(p)
        }
    }

    fun pause() {
        playWhenReady = false
        val p = player ?: return
        if (playing) runCatching { p.pause() }
        playing = false
        report(force = true)
    }

    fun seekTo(ms: Long) {
        val clamped = ms.coerceIn(0, if (durationMs > 0) durationMs else Long.MAX_VALUE)
        positionMs = clamped
        player?.takeIf { phase == Phase.Ready }?.let { runCatching { it.seekTo(clamped.toInt()) } }
    }

    fun skip(seconds: Int) = seekTo(positionMs + seconds * 1000L)

    fun cycleSpeed() {
        speed = com.redvaultsystems.redvaultmessage.data.model.ChannelPosts.nextSpeed(speed)
        // Setting a speed starts a paused MediaPlayer, so it's applied only while playing (and on the next Play).
        val p = player ?: return
        if (playing) runCatching { p.playbackParams = p.playbackParams.setSpeed(speed) }
    }

    fun setSurface(target: Surface?) {
        surface = target
        player?.let { runCatching { it.setSurface(target) } }
    }

    /** Called about every 250 ms by the screen while playing. */
    fun tick() {
        val p = player ?: return
        if (phase != Phase.Ready) return
        positionMs = runCatching { p.currentPosition.toLong() }.getOrDefault(positionMs)
        report(force = false)
    }

    fun release() {
        report(force = true)
        NowPlaying.drop(this)
        player?.let { runCatching { it.release() } }
        player = null
        playing = false
        phase = Phase.Idle
    }

    private fun prepare() {
        player?.let { runCatching { it.release() } }
        val data = source() ?: run {
            phase = Phase.Failed
            return
        }
        val p = MediaPlayer()
        player = p
        phase = Phase.Preparing
        p.setOnPreparedListener {
            if (player !== p) return@setOnPreparedListener
            durationMs = p.duration.toLong().coerceAtLeast(0)
            videoWidth = p.videoWidth
            videoHeight = p.videoHeight
            phase = Phase.Ready
            val resume = resumeAtSeconds * 1000L
            if (positionMs > 0) runCatching { p.seekTo(positionMs.toInt()) }
            else if (resumeAtSeconds > 5 && (durationMs <= 0 || resume < durationMs - 5000)) {
                positionMs = resume
                runCatching { p.seekTo(resume.toInt()) }
            }
            if (playWhenReady) start(p)
        }
        p.setOnVideoSizeChangedListener { _, w, h ->
            videoWidth = w
            videoHeight = h
        }
        p.setOnCompletionListener {
            playing = false
            positionMs = 0
            runCatching { p.seekTo(0) }
            onPosition?.invoke(0)
            lastReported = 0
        }
        p.setOnErrorListener { _, _, _ ->
            if (player === p) {
                phase = Phase.Failed
                playing = false
            }
            true
        }
        try {
            p.setDataSource(data)
            surface?.let(p::setSurface)
            p.prepareAsync()
        } catch (e: Exception) {
            phase = Phase.Failed
        }
    }

    private fun start(p: MediaPlayer) {
        playWhenReady = false
        try {
            if (speed != 1f) p.playbackParams = p.playbackParams.setSpeed(speed)
            p.start()
            playing = true
        } catch (e: Exception) {
            phase = Phase.Failed
            playing = false
        }
    }

    private fun report(force: Boolean) {
        val callback = onPosition ?: return
        if (phase != Phase.Ready) return
        val at = positionMs
        if (force || kotlin.math.abs(at - lastReported) >= 5000) {
            lastReported = at
            callback((at / 1000).toInt())
        }
    }
}
