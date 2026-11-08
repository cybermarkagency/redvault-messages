// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.calls

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.Ringtone
import android.media.RingtoneManager
import android.media.ToneGenerator
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager

/**
 * A call's sound: voice routing (earpiece or speaker), audio focus, the
 * incoming ringtone with vibration, and the ringback tone while calling out.
 */
class CallAudio(private val context: Context) {
    private val manager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var focus: AudioFocusRequest? = null
    private var previousMode = AudioManager.MODE_NORMAL
    private var active = false

    private var ringtone: Ringtone? = null
    private var tone: ToneGenerator? = null

    /** Switches the phone into a call: voice-call audio, focus, and the earpiece or speaker. */
    fun start(speaker: Boolean) {
        stopTones()
        if (!active) {
            active = true
            previousMode = manager.mode
            val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build(),
                )
                .build()
            manager.requestAudioFocus(request)
            focus = request
            manager.mode = AudioManager.MODE_IN_COMMUNICATION
        }
        setSpeaker(speaker)
    }

    fun setSpeaker(on: Boolean) {
        if (Build.VERSION.SDK_INT >= 31) {
            val wanted = if (on) AudioDeviceInfo.TYPE_BUILTIN_SPEAKER else AudioDeviceInfo.TYPE_BUILTIN_EARPIECE
            val device = manager.availableCommunicationDevices.firstOrNull { it.type == wanted }
            if (device != null) manager.setCommunicationDevice(device) else if (!on) manager.clearCommunicationDevice()
        } else {
            @Suppress("DEPRECATION")
            manager.isSpeakerphoneOn = on
        }
    }

    /** The incoming ringtone (the phone's own), with vibration. */
    fun ring() {
        stopTones()
        val uri = RingtoneManager.getActualDefaultRingtoneUri(context, RingtoneManager.TYPE_RINGTONE)
            ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE)
        ringtone = RingtoneManager.getRingtone(context, uri)?.apply {
            if (Build.VERSION.SDK_INT >= 28) isLooping = true
            audioAttributes = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE).build()
            runCatching { play() }
        }
        vibrator()?.vibrate(VibrationEffect.createWaveform(longArrayOf(0, 800, 1200), 0))
    }

    /** The ringback tone the caller hears while it rings at the other end. */
    fun ringback() {
        stopTones()
        tone = runCatching { ToneGenerator(AudioManager.STREAM_VOICE_CALL, 70) }.getOrNull()?.apply { startTone(ToneGenerator.TONE_SUP_RINGTONE) }
    }

    fun stopTones() {
        ringtone?.stop()
        ringtone = null
        tone?.stopTone()
        tone?.release()
        tone = null
        vibrator()?.cancel()
    }

    /** Back to normal audio once the call ends. */
    fun stop() {
        stopTones()
        if (!active) return
        active = false
        if (Build.VERSION.SDK_INT >= 31) manager.clearCommunicationDevice() else {
            @Suppress("DEPRECATION")
            manager.isSpeakerphoneOn = false
        }
        manager.mode = previousMode
        focus?.let { manager.abandonAudioFocusRequest(it) }
        focus = null
    }

    private fun vibrator(): Vibrator? = if (Build.VERSION.SDK_INT >= 31) {
        (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)?.defaultVibrator
    } else {
        @Suppress("DEPRECATION")
        context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
    }
}
