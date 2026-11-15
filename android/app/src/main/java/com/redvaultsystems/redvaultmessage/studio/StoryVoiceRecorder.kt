// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.studio

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Handler
import android.os.Looper
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

/**
 * Records a voice for a story as raw sound (16-bit mono), so it can be edited with [AudioDsp] and written as a WAV exactly as the web's studio
 * does. The microphone is only opened by [start], which needs the RECORD_AUDIO permission to have been granted first; nothing is kept but what is
 * recorded, and nothing leaves the phone to record or edit it. It stops by itself at the length a story allows.
 */
class StoryVoiceRecorder {
    private var record: AudioRecord? = null
    private var thread: Thread? = null
    private val chunks = ArrayList<ShortArray>()
    private val main = Handler(Looper.getMainLooper())

    @Volatile private var running = false
    @Volatile private var samples = 0L

    /** How loud the last moment was, 0 to 1, for a level meter. */
    @Volatile var level: Float = 0f
        private set

    /** The rate it records at (22.05 kHz where the phone allows: voices don't need more). */
    var sampleRate: Int = 22_050
        private set

    val elapsedMs: Long get() = samples * 1000 / sampleRate

    /** Opens the microphone and starts. False when it can't be opened (no microphone, or someone else has it). [onLimit] runs on the main thread when the length is reached. */
    @SuppressLint("MissingPermission")
    fun start(maxMs: Long, onLimit: () -> Unit): Boolean {
        for (rate in intArrayOf(22_050, 44_100, 16_000)) {
            val minBuffer = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            if (minBuffer <= 0) continue
            val buffer = max(minBuffer, rate / 5 * 2)
            val rec = try {
                AudioRecord(MediaRecorder.AudioSource.MIC, rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, buffer)
            } catch (e: Exception) {
                continue
            }
            if (rec.state != AudioRecord.STATE_INITIALIZED) {
                rec.release()
                continue
            }
            try {
                rec.startRecording()
            } catch (e: Exception) {
                rec.release()
                continue
            }
            record = rec
            sampleRate = rate
            samples = 0
            chunks.clear()
            running = true
            val limit = maxMs * rate / 1000
            thread = Thread {
                val read = ShortArray(buffer / 2)
                while (running) {
                    val n = rec.read(read, 0, read.size)
                    if (n <= 0) continue
                    synchronized(chunks) { chunks.add(read.copyOf(n)) }
                    var sum = 0.0
                    for (i in 0 until n) sum += (read[i] / 32768.0) * (read[i] / 32768.0)
                    level = min(1f, (sqrt(sum / n) * 4).toFloat())
                    samples += n
                    if (samples >= limit) {
                        running = false
                        main.post(onLimit)
                    }
                }
            }.also { it.start() }
            return true
        }
        return false
    }

    /** Stops and returns what was recorded (the sound at [sampleRate]); null if nothing was. The microphone is let go. */
    fun stop(): Mono? {
        running = false
        thread?.join(1000)
        thread = null
        record?.let { runCatching { it.stop() }; it.release() }
        record = null
        level = 0f
        val all = synchronized(chunks) { chunks.toList().also { chunks.clear() } }
        val total = all.sumOf { it.size }
        if (total == 0) return null
        val out = FloatArray(total)
        var at = 0
        for (c in all) {
            for (s in c) out[at++] = s / 32768f
        }
        return Mono(out, sampleRate.toDouble())
    }

    /** Throws the recording away and lets go of the microphone. */
    fun cancel() {
        stop()
    }
}
