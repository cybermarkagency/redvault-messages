// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.studio

import android.content.Context
import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import com.redvaultsystems.redvaultmessage.data.StoryInputException
import com.redvaultsystems.redvaultmessage.data.model.StoryLimits
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.nio.ByteOrder

/**
 * Opens an audio file the person chose (MP3, M4A, WAV, OGG and the rest the phone can play) as mono sound for the voice studio, brought down to
 * the rate a story keeps. Its length is read from its header first, so a long file is refused before it is decoded.
 */
object AudioFileDecoder {
    suspend fun decode(context: Context, uri: Uri): Mono = withContext(Dispatchers.IO) {
        // A WAV this app wrote (16-bit mono) is read directly; anything else goes through the phone's decoders.
        runCatching {
            context.contentResolver.openInputStream(uri)?.use { input ->
                val head = input.readNBytes(12)
                if (head.size == 12 && String(head, 0, 4, Charsets.ISO_8859_1) == "RIFF" && String(head, 8, 4, Charsets.ISO_8859_1) == "WAVE") {
                    val rest = input.readNBytes(MAX_WAV_BYTES - 12 + 1)
                    if (rest.size + 12 <= MAX_WAV_BYTES) AudioDsp.decodeWav(head + rest) else null
                } else {
                    null
                }
            }
        }.getOrNull()?.let { return@withContext checked(shrink(it)) }
        val extractor = MediaExtractor()
        try {
            try {
                extractor.setDataSource(context, uri, null)
            } catch (e: Exception) {
                throw StoryInputException("That audio couldn't be opened.")
            }
            var track = -1
            for (i in 0 until extractor.trackCount) {
                if (extractor.getTrackFormat(i).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true) {
                    track = i
                    break
                }
            }
            if (track < 0) throw StoryInputException("That isn't an audio file.")
            val format = extractor.getTrackFormat(track)
            if (format.containsKey(MediaFormat.KEY_DURATION) && format.getLong(MediaFormat.KEY_DURATION) / 1000 > StoryLimits.MAX_AUDIO_MS + 500) {
                throw StoryInputException("Audio stories can be up to 5 minutes long. Trim the file first.")
            }
            extractor.selectTrack(track)
            checked(shrink(decodeTrack(extractor, format)))
        } finally {
            extractor.release()
        }
    }

    /** A voice doesn't need CD quality, and it keeps what is held in memory small (the web's studio does the same). */
    private fun shrink(m: Mono): Mono = if (m.sampleRate > AudioDsp.STORY_AUDIO_RATE) AudioDsp.resample(m, AudioDsp.STORY_AUDIO_RATE) else m

    private fun checked(m: Mono): Mono {
        val ms = AudioDsp.durationMs(m)
        if (ms < 300) throw StoryInputException("That recording is too short.")
        return m
    }

    private fun decodeTrack(extractor: MediaExtractor, format: MediaFormat): Mono {
        val mime = format.getString(MediaFormat.KEY_MIME) ?: throw StoryInputException("That audio couldn't be opened.")
        val codec = try {
            MediaCodec.createDecoderByType(mime)
        } catch (e: Exception) {
            throw StoryInputException("This phone can't open that kind of audio.")
        }
        val chunks = ArrayList<FloatArray>()
        var total = 0L
        var rate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
        var channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
        var floatPcm = false
        try {
            codec.configure(format, null, null, 0)
            codec.start()
            val info = MediaCodec.BufferInfo()
            var inputDone = false
            var outputDone = false
            // About six minutes of sound is the most that is read; the length check above has already refused longer files that say how long they are.
            val limit = 6L * 60 * 48_000
            while (!outputDone) {
                if (!inputDone) {
                    val index = codec.dequeueInputBuffer(10_000)
                    if (index >= 0) {
                        val buffer = codec.getInputBuffer(index)!!
                        val size = extractor.readSampleData(buffer, 0)
                        if (size < 0) {
                            codec.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        } else {
                            codec.queueInputBuffer(index, 0, size, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                val out = codec.dequeueOutputBuffer(info, 10_000)
                when {
                    out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val f = codec.outputFormat
                        rate = f.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                        channels = f.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                        floatPcm = f.containsKey(MediaFormat.KEY_PCM_ENCODING) && f.getInteger(MediaFormat.KEY_PCM_ENCODING) == AudioFormat.ENCODING_PCM_FLOAT
                    }
                    out >= 0 -> {
                        val buffer = codec.getOutputBuffer(out)!!
                        buffer.position(info.offset)
                        buffer.limit(info.offset + info.size)
                        buffer.order(ByteOrder.nativeOrder())
                        val frames = info.size / ((if (floatPcm) 4 else 2) * channels)
                        val mono = FloatArray(frames)
                        for (f in 0 until frames) {
                            var sum = 0f
                            for (c in 0 until channels) sum += if (floatPcm) buffer.getFloat() else buffer.getShort() / 32768f
                            mono[f] = sum / channels
                        }
                        chunks.add(mono)
                        total += frames
                        codec.releaseOutputBuffer(out, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) outputDone = true
                        if (total > limit) throw StoryInputException("Audio stories can be up to 5 minutes long. Trim the file first.")
                    }
                }
            }
        } catch (e: StoryInputException) {
            throw e
        } catch (e: Exception) {
            throw StoryInputException("That audio couldn't be opened.")
        } finally {
            runCatching { codec.stop() }
            codec.release()
        }
        val all = FloatArray(total.toInt())
        var at = 0
        for (c in chunks) {
            System.arraycopy(c, 0, all, at, c.size)
            at += c.size
        }
        return Mono(all, rate.toDouble())
    }

    /** A WAV file is a few seconds to a few minutes of 16-bit sound; more than this is not read as one. */
    private const val MAX_WAV_BYTES = 30 * 1024 * 1024
}
