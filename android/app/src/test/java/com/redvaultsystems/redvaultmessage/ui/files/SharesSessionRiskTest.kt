// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SharesSessionRiskTest {
    private val rlo = Char(0x202E).toString()
    private val zeroWidth = Char(0x200B).toString()

    @Test
    fun onlyInertTypesSaveWithoutAWarning() {
        for (name in listOf("photo.jpg", "clip.MP4", "notes.txt", "scan.pdf", "song.flac")) {
            assertFalse(name, SharesSession.isRisky(name))
        }
    }

    @Test
    fun everythingElseWarns() {
        // The old deny-list read the raw name: a trailing dot, space or invisible character hid the type,
        // and types it didn't list (rdp, py, appinstaller...) never warned at all.
        val names = listOf(
            "invoice.apk", "invoice.apk.", "invoice.apk ", "invoice.exe$rlo", "invoice.exe$zeroWidth", "page.html", "x.svg",
            "remote.rdp", "script.py", "app.appinstaller", "noextension", ".pdf",
        )
        for (name in names) assertTrue(name, SharesSession.isRisky(name))
    }

    @Test
    fun cutNamesNeverEndInASpaceOrDot() {
        assertEquals("x".repeat(149), FileMessage.sanitizeFilename("x".repeat(149) + " " + "y".repeat(50)))
        assertFalse(FileMessage.sanitizeFilename("x".repeat(149) + "." + "y".repeat(50)).endsWith("."))
    }
}
