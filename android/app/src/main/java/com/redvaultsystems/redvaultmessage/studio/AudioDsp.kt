// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.studio

import com.redvaultsystems.redvaultmessage.data.model.jsRound
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tanh

/** Mono sound: samples from -1 to 1 and the rate they were made at. */
class Mono(val samples: FloatArray, val sampleRate: Double)

/** The voice filters of a voice story (the web's names). */
enum class VoiceFilter(val wire: String, val label: String) {
    None("none", "Normal"), Chipmunk("chipmunk", "Chipmunk"), Deep("deep", "Deep"), Robot("robot", "Robot"), Echo("echo", "Echo"), Radio("radio", "Radio"),
}

/** What a finished voice is: a WAV file, how long it is, and the bars that draw its loudness. */
class FinishedVoice(val wav: ByteArray, val durationMs: Long, val waveform: List<Int>)

/**
 * Audio editing, all on this phone: trim, fade, even out, change the voice, draw a picture of the loudness and write a WAV. A port of
 * ui/src/lib/studio/audioDsp.ts, sample for sample (the signal maths runs in doubles and is stored as 32-bit floats, as it is in JavaScript),
 * pinned to what the web's code makes by studio_vectors.json. Nothing here touches the network.
 */
object AudioDsp {
    /** What a story keeps: voices don't need CD quality, and the file stays small. */
    const val STORY_AUDIO_RATE = 22_050.0

    private fun clamp(v: Double, lo: Double, hi: Double) = min(hi, max(lo, v))
    private fun clampInt(v: Long, lo: Long, hi: Long) = min(hi, max(lo, v)).toInt()

    fun durationMs(m: Mono): Long = jsRound(m.samples.size / m.sampleRate * 1000)

    fun mixToMono(channels: List<FloatArray>, sampleRate: Double): Mono {
        if (channels.isEmpty()) return Mono(FloatArray(0), sampleRate)
        if (channels.size == 1) return Mono(channels[0].copyOf(), sampleRate)
        val length = channels.minOf { it.size }
        val out = FloatArray(length)
        for (i in 0 until length) {
            var sum = 0.0
            for (c in channels) sum += c[i]
            out[i] = (sum / channels.size).toFloat()
        }
        return Mono(out, sampleRate)
    }

    // --- Editing ----------------------------------------------------------------------------------------------

    /** Linear resampling (good enough for speech). */
    fun resample(m: Mono, toRate: Double): Mono {
        if (toRate == m.sampleRate || m.samples.isEmpty()) return Mono(m.samples.copyOf(), toRate)
        val ratio = m.sampleRate / toRate
        val length = max(1, floor(m.samples.size / ratio).toInt())
        val out = FloatArray(length)
        for (i in 0 until length) {
            val pos = i * ratio
            val i0 = floor(pos).toInt()
            val frac = pos - i0
            val a = m.samples[i0].toDouble()
            val b = m.samples[min(i0 + 1, m.samples.size - 1)].toDouble()
            out[i] = (a + (b - a) * frac).toFloat()
        }
        return Mono(out, toRate)
    }

    /** Keeps [startMs] to [endMs] (clamped to the recording). */
    fun trim(m: Mono, startMs: Double, endMs: Double): Mono {
        val n = m.samples.size.toLong()
        val start = clampInt(jsRound(startMs / 1000 * m.sampleRate), 0, n)
        val end = clampInt(jsRound(endMs / 1000 * m.sampleRate), start.toLong(), n)
        return Mono(m.samples.copyOfRange(start, end), m.sampleRate)
    }

    /** Fades the start in and the end out so a cut doesn't click. */
    fun fade(m: Mono, inMs: Double, outMs: Double): Mono {
        val out = m.samples.copyOf()
        val inN = min(out.size.toLong(), jsRound(inMs / 1000 * m.sampleRate)).toInt()
        val outN = min(out.size.toLong(), jsRound(outMs / 1000 * m.sampleRate)).toInt()
        for (i in 0 until inN) out[i] = (out[i].toDouble() * (i.toDouble() / inN)).toFloat()
        for (i in 0 until outN) out[out.size - 1 - i] = (out[out.size - 1 - i].toDouble() * (i.toDouble() / outN)).toFloat()
        return Mono(out, m.sampleRate)
    }

    fun peak(samples: FloatArray): Double {
        var p = 0.0
        for (s in samples) p = max(p, abs(s.toDouble()))
        return p
    }

    /** Scales the recording so its loudest point is [target] (never boosts silence into noise). */
    fun normalize(m: Mono, target: Double = 0.95): Mono {
        val p = peak(m.samples)
        if (p < 1e-4) return Mono(m.samples.copyOf(), m.sampleRate)
        val gain = target / p
        return Mono(FloatArray(m.samples.size) { (m.samples[it] * gain).toFloat() }, m.sampleRate)
    }

    fun gain(m: Mono, factor: Double): Mono =
        Mono(FloatArray(m.samples.size) { clamp(m.samples[it] * factor, -1.0, 1.0).toFloat() }, m.sampleRate)

    // --- Voice filters -------------------------------------------------------------------------------------------

    /** A pitch change by playing faster or slower, so the length changes with it (a toy effect, on purpose). */
    private fun repitch(m: Mono, factor: Double): Mono = Mono(resample(m, m.sampleRate / factor).samples, m.sampleRate)

    private fun ringModulate(m: Mono, hz: Double): Mono =
        Mono(FloatArray(m.samples.size) { (m.samples[it] * sin((2 * PI * hz * it) / m.sampleRate)).toFloat() }, m.sampleRate)

    private fun echo(m: Mono, delayMs: Double, feedback: Double, mix: Double): Mono {
        val delay = max(1L, jsRound(delayMs / 1000 * m.sampleRate)).toInt()
        val tail = delay * 6
        val out = FloatArray(m.samples.size + tail)
        val wet = FloatArray(out.size)
        for (i in out.indices) {
            val dry = if (i < m.samples.size) m.samples[i].toDouble() else 0.0
            val echoed = if (i >= delay) wet[i - delay] * feedback else 0.0
            wet[i] = (dry + echoed).toFloat()
            out[i] = (dry * (1 - mix * 0.5) + (wet[i] - dry) * mix).toFloat()
        }
        return Mono(out, m.sampleRate)
    }

    /** A second-order filter: the state is kept in doubles, the result is stored as floats (as the web does). */
    private fun biquad(samples: FloatArray, c: DoubleArray): FloatArray {
        val out = FloatArray(samples.size)
        var x1 = 0.0
        var x2 = 0.0
        var y1 = 0.0
        var y2 = 0.0
        for (i in samples.indices) {
            val x0 = samples[i].toDouble()
            val y0 = c[0] * x0 + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2
            out[i] = y0.toFloat()
            x2 = x1
            x1 = x0
            y2 = y1
            y1 = y0
        }
        return out
    }

    private fun lowpass(rate: Double, hz: Double): DoubleArray {
        val w0 = (2 * PI * hz) / rate
        val alpha = sin(w0) / (2 * 0.7071)
        val c = cos(w0)
        val a0 = 1 + alpha
        return doubleArrayOf((1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, (-2 * c) / a0, (1 - alpha) / a0)
    }

    private fun highpass(rate: Double, hz: Double): DoubleArray {
        val w0 = (2 * PI * hz) / rate
        val alpha = sin(w0) / (2 * 0.7071)
        val c = cos(w0)
        val a0 = 1 + alpha
        return doubleArrayOf((1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, (-2 * c) / a0, (1 - alpha) / a0)
    }

    private fun radio(m: Mono): Mono {
        val hp = highpass(m.sampleRate, 400.0)
        val lp = lowpass(m.sampleRate, min(3200.0, m.sampleRate / 2.5))
        val s = biquad(biquad(m.samples, hp), lp)
        // A little grit: soft clipping.
        return Mono(FloatArray(s.size) { (tanh(s[it] * 2.2) * 0.8).toFloat() }, m.sampleRate)
    }

    /** Applies a voice filter. The result may be a different length (chipmunk, deep, echo). */
    fun applyVoiceFilter(m: Mono, filter: VoiceFilter): Mono = when (filter) {
        VoiceFilter.None -> Mono(m.samples.copyOf(), m.sampleRate)
        VoiceFilter.Chipmunk -> repitch(m, 1.4)
        VoiceFilter.Deep -> repitch(m, 0.75)
        VoiceFilter.Robot -> normalize(ringModulate(m, 55.0), 0.9)
        VoiceFilter.Echo -> normalize(echo(m, 240.0, 0.45, 0.6), 0.95)
        VoiceFilter.Radio -> radio(m)
    }

    // --- Picture of the sound ----------------------------------------------------------------------------------------

    /** [bars] loudness values, 0 to 100, from the recording's volume (RMS per slice, scaled to the loudest slice). */
    fun waveformBars(samples: FloatArray, bars: Int = 48): List<Int> {
        if (samples.isEmpty()) return List(bars) { 0 }
        val rms = DoubleArray(bars)
        for (b in 0 until bars) {
            val start = ((b.toLong() * samples.size) / bars).toInt()
            val end = max(start + 1, (((b + 1).toLong() * samples.size) / bars).toInt())
            var sum = 0.0
            var i = start
            while (i < end && i < samples.size) {
                val v = samples[i].toDouble()
                sum += v * v
                i++
            }
            rms[b] = sqrt(sum / (end - start))
        }
        val top = max(rms.max(), 1e-6)
        return rms.map { jsRound(clamp(it / top, 0.0, 1.0) * 100).toInt() }
    }

    // --- WAV -----------------------------------------------------------------------------------------------------------

    /** 16-bit mono PCM in a WAV file. */
    fun encodeWav(m: Mono): ByteArray {
        val n = m.samples.size
        val buffer = ByteBuffer.allocate(44 + n * 2).order(ByteOrder.LITTLE_ENDIAN)
        fun text(s: String) = s.forEach { buffer.put(it.code.toByte()) }
        text("RIFF")
        buffer.putInt(36 + n * 2)
        text("WAVE")
        text("fmt ")
        buffer.putInt(16)
        buffer.putShort(1) // PCM
        buffer.putShort(1) // mono
        buffer.putInt(m.sampleRate.toLong().toInt())
        buffer.putInt((m.sampleRate * 2).toLong().toInt())
        buffer.putShort(2)
        buffer.putShort(16)
        text("data")
        buffer.putInt(n * 2)
        for (sample in m.samples) {
            val s = clamp(sample.toDouble(), -1.0, 1.0)
            buffer.putShort((if (s < 0) s * 0x8000 else s * 0x7fff).toInt().toShort())
        }
        return buffer.array()
    }

    /** Reads a WAV this module wrote (16-bit mono PCM). Returns null for anything else. */
    fun decodeWav(bytes: ByteArray): Mono? {
        if (bytes.size < 44) return null
        val view = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        fun tag(o: Int) = String(CharArray(4) { (bytes[o + it].toInt() and 0xff).toChar() })
        if (tag(0) != "RIFF" || tag(8) != "WAVE" || tag(12) != "fmt " || tag(36) != "data") return null
        if (view.getShort(20).toInt() != 1 || view.getShort(22).toInt() != 1 || view.getShort(34).toInt() != 16) return null
        val declared = (view.getInt(40).toLong() and 0xffffffffL) / 2.0
        val n = min(declared, floor((bytes.size - 44) / 2.0)).toInt()
        val samples = FloatArray(n)
        for (i in 0 until n) {
            val v = view.getShort(44 + i * 2).toInt()
            samples[i] = (if (v < 0) v / 32768.0 else v / 32767.0).toFloat()
        }
        return Mono(samples, (view.getInt(24).toLong() and 0xffffffffL).toDouble())
    }

    /** The whole chain for a story: trim, filter, even out, fade the cut, bring the rate down, write a WAV. */
    fun finishVoice(m: Mono, startMs: Double, endMs: Double, filter: VoiceFilter): FinishedVoice {
        var work = trim(m, startMs, endMs)
        work = applyVoiceFilter(work, filter)
        work = resample(work, min(work.sampleRate, STORY_AUDIO_RATE))
        work = fade(normalize(work, 0.92), 40.0, 120.0)
        return FinishedVoice(encodeWav(work), durationMs(work), waveformBars(work.samples, 48))
    }
}
