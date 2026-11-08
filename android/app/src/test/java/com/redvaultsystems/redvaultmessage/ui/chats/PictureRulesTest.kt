// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import com.redvaultsystems.redvaultmessage.ui.chats.ChatTestData.file
import com.redvaultsystems.redvaultmessage.ui.chats.ChatTestData.text
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PictureRulesTest {
    @Test
    fun theDesktopsPictureTypesShowInline() {
        for (type in listOf("image/jpeg", "image/png", "image/gif", "image/webp", "image/bmp", "IMAGE/JPEG", "image/png; charset=binary")) {
            assertTrue(type, PictureRules.isInlineImage(file("1", type)))
        }
        for (type in listOf("image/svg+xml", "image/heic", "video/mp4", "application/pdf", "audio/ogg")) {
            assertFalse(type, PictureRules.isInlineImage(file("1", type)))
        }
    }

    @Test
    fun onlyAvailableFilesAndNothingStillQueued() {
        assertTrue(PictureRules.isInlineImage(file("1", "image/png", status = "available")))
        assertFalse(PictureRules.isInlineImage(file("1", "image/png", status = "expired")))
        assertFalse(PictureRules.isInlineImage(file("1", "image/png", status = "deleted")))
        assertFalse(PictureRules.isInlineImage(file("1", "image/png").copy(queued = true)))
        assertFalse(PictureRules.isInlineImage(text("1", 1)))
    }

    @Test
    fun aHugePictureStaysAFile() {
        assertTrue(PictureRules.isInlineImage(file("1", "image/jpeg", size = PictureRules.MAX_INLINE_BYTES)))
        assertFalse(PictureRules.isInlineImage(file("1", "image/jpeg", size = PictureRules.MAX_INLINE_BYTES + 1)))
    }

    @Test
    fun sampleSizeKeepsTheLongerSideAtOrAboveTheTarget() {
        assertEquals(1, PictureRules.sampleSize(800, 600, 720))
        assertEquals(2, PictureRules.sampleSize(1440, 900, 720))
        assertEquals(4, PictureRules.sampleSize(4000, 3000, 720))
        assertEquals(4, PictureRules.sampleSize(3000, 4000, 720))
        assertEquals(1, PictureRules.sampleSize(0, 0, 720))
    }
}
