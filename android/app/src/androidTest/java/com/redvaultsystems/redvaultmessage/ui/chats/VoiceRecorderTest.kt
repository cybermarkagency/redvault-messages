// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import android.Manifest
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.rule.GrantPermissionRule
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/** Records for real on the device's microphone. */
@RunWith(AndroidJUnit4::class)
class VoiceRecorderTest {
    @get:Rule
    val mic: GrantPermissionRule = GrantPermissionRule.grant(Manifest.permission.RECORD_AUDIO)

    private val recorder = VoiceRecorder(InstrumentationRegistry.getInstrumentation().targetContext)

    @Test
    fun recordsAPlayableMp4AudioFile() {
        InstrumentationRegistry.getInstrumentation().runOnMainSync { recorder.start(onLimitReached = {}) }
        Thread.sleep(1_500)
        var file: java.io.File? = null
        InstrumentationRegistry.getInstrumentation().runOnMainSync { file = recorder.stop() }
        val recording = file
        assertNotNull(recording)
        recording!!
        try {
            assertTrue("recording is empty", recording.length() > 1_000)
            // An MP4 container starts with a box whose type is "ftyp".
            val header = recording.inputStream().use { it.readNBytes(8) }
            assertEquals("ftyp", String(header, 4, 4, Charsets.US_ASCII))
        } finally {
            recording.delete()
        }
    }

    @Test
    fun aTapTooShortToBeAMessageKeepsNothing() {
        InstrumentationRegistry.getInstrumentation().runOnMainSync { recorder.start(onLimitReached = {}) }
        var file: java.io.File? = java.io.File("placeholder")
        InstrumentationRegistry.getInstrumentation().runOnMainSync { file = recorder.stop() }
        assertNull(file)
    }
}
