// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.components

import org.junit.Assert.assertEquals
import org.junit.Test

class WindowProtectionTest {
    private val changes = mutableListOf<Boolean>()
    private val holds = SecureHolds { changes += it }

    @Test
    fun leavingASecretScreenDoesNotUndoTheSetting() {
        holds.hold() // "Hide app content" is on
        holds.hold() // a recovery key screen opens
        holds.release() // and closes
        assertEquals(listOf(true), changes)
        holds.release()
        assertEquals(listOf(true, false), changes)
    }

    @Test
    fun aStrayReleaseCantMakeTheNextHoldCountForNothing() {
        holds.release()
        holds.hold()
        assertEquals(listOf(true), changes)
    }

    @Test
    fun androidThirteenKeepsScreenshotsAndOlderVersionsCant() {
        assertEquals(RecentsProtection.None, RecentsProtection.forDevice(35, hide = false))
        assertEquals(RecentsProtection.None, RecentsProtection.forDevice(26, hide = false))
        assertEquals(RecentsProtection.NoThumbnail, RecentsProtection.forDevice(33, hide = true))
        assertEquals(RecentsProtection.SecureWindow, RecentsProtection.forDevice(32, hide = true))
        assertEquals(RecentsProtection.SecureWindow, RecentsProtection.forDevice(26, hide = true))
    }
}
