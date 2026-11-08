// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import android.content.Context
import android.media.MediaRecorder
import android.os.Build
import android.os.SystemClock
import java.io.File
import java.util.UUID

/**
 * Records one voice message (AAC in an MP4 container, which browsers can
 * play) into the app's private cache. The recording is deleted after it's
 * sent or cancelled; it's never kept.
 */
class VoiceRecorder(private val context: Context, private val maxDurationMs: Int = MAX_DURATION_MS) {
    private var recorder: MediaRecorder? = null
    private var file: File? = null
    private var startedAt = 0L

    val isRecording: Boolean get() = recorder != null

    fun elapsedMs(): Long = if (isRecording) SystemClock.elapsedRealtime() - startedAt else 0L

    /** Starts recording; [onLimitReached] fires if it runs to the maximum length. */
    fun start(onLimitReached: () -> Unit) {
        val dir = File(context.cacheDir, "outgoing").apply { mkdirs() }
        val target = File(dir, "voice-${UUID.randomUUID()}.m4a")
        @Suppress("DEPRECATION")
        val r = if (Build.VERSION.SDK_INT >= 31) MediaRecorder(context) else MediaRecorder()
        try {
            r.setAudioSource(MediaRecorder.AudioSource.MIC)
            r.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
            r.setAudioEncoder(MediaRecorder.AudioEncoder.AAC)
            r.setAudioChannels(1)
            r.setAudioSamplingRate(44_100)
            r.setAudioEncodingBitRate(64_000)
            r.setMaxDuration(maxDurationMs)
            r.setOnInfoListener { _, what, _ ->
                if (what == MediaRecorder.MEDIA_RECORDER_INFO_MAX_DURATION_REACHED) onLimitReached()
            }
            r.setOutputFile(target.absolutePath)
            r.prepare()
            r.start()
        } catch (e: Exception) {
            r.release()
            target.delete()
            throw e
        }
        recorder = r
        file = target
        startedAt = SystemClock.elapsedRealtime()
    }

    /** Stops and returns the recording, or null if it was too short to keep. */
    fun stop(): File? {
        val r = recorder ?: return null
        val f = file
        val long = elapsedMs() >= MIN_DURATION_MS
        recorder = null
        file = null
        val ok = runCatching { r.stop() }.isSuccess
        r.release()
        return if (ok && long) f else f?.delete().let { null }
    }

    fun cancel() {
        stop()?.delete()
    }

    companion object {
        const val MAX_DURATION_MS = 5 * 60 * 1000
        private const val MIN_DURATION_MS = 700L
    }
}
