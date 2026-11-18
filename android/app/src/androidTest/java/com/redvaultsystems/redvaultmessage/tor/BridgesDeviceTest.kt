// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import android.graphics.Bitmap
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextInput
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.redvaultsystems.redvaultmessage.ui.settings.NetworkRoutingScreen
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import java.io.File
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.flow.first
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Bridges on a device: Settings › Network routing's real screen over the real TorController, with the bridge
 * programs (liblyrebird.so, libsnowflake.so) from this installed app's native-library folder. Needs the internet:
 * it connects to the live onion service through Snowflake. Screenshots go to cache/bridges-shots
 * (adb exec-out run-as com.redvaultsystems.redvaultmessage cat cache/bridges-shots/<name>.png).
 */
@RunWith(AndroidJUnit4::class)
class BridgesDeviceTest {
    @get:Rule val rule = createComposeRule()

    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private val dataDir = File(context.cacheDir, "bridges-test-tor").apply { deleteRecursively() }
    private val tor = TorController(
        ONION, dataDir, nativeLibDir = File(context.applicationInfo.nativeLibraryDir),
    )

    @After fun stop() {
        tor.stop()
    }

    private fun shoot(name: String) {
        val out = File(context.cacheDir, "bridges-shots").apply { mkdirs() }
        File(out, "$name.png").outputStream().use { rule.onRoot().captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    private fun show() {
        rule.setContent { RedvaultTheme { NetworkRoutingScreen(tor = tor, queued = 0, onBack = {}) } }
    }

    @Test fun theProgramsAreInstalledAndRunnable() {
        val dir = File(context.applicationInfo.nativeLibraryDir)
        for (name in listOf("liblyrebird.so", "libsnowflake.so")) {
            val program = File(dir, name)
            assertTrue("$name is in ${dir.path}", program.isFile)
            assertTrue("$name can be run", program.canExecute())
        }
        assertTrue(tor.bridgesAvailable)
    }

    @Test fun pastedGarbageIsRefusedOnScreenAndNothingChanges() {
        show()
        rule.onNodeWithText("Bridges I enter myself").performScrollTo().performClick()
        rule.onNode(hasText("obfs4 192.0.2.1:443", substring = true)).performTextInput("obfs4 not-an-address")
        rule.onNodeWithText("Save and reconnect").performScrollTo().performClick()
        rule.waitUntil(5_000) { rule.onAllNodes(hasText("isn't a valid bridge line", substring = true)).fetchSemanticsNodes().isNotEmpty() }
        assertEquals(TorController.BRIDGES_OFF, tor.bridgeMode.value)
        shoot("custom-refused")
    }

    @Test fun snowflakeFromTheScreenConnectsToTheLiveService() {
        tor.start()
        show()
        shoot("off")
        rule.onNodeWithText("Snowflake (looks like a video call)").performScrollTo().performClick()
        rule.onNodeWithText("Save and reconnect").performScrollTo().assertIsEnabled().performClick()
        rule.waitUntil(5_000) { rule.onAllNodes(hasText("Saved. Tor is reconnecting", substring = true)).fetchSemanticsNodes().isNotEmpty() }
        assertEquals("snowflake", tor.bridgeMode.value)
        shoot("snowflake-saved")
        // Snowflake takes one to three minutes.
        val connected = runBlocking { withTimeoutOrNull(300_000) { tor.status.first { it.state == TorState.Connected } } }
        assertTrue("Tor connected through Snowflake (last: ${tor.status.value})", connected != null)
        rule.waitForIdle()
        shoot("snowflake-connected")
    }

    private companion object {
        const val ONION = "4cydjhdc3lmhg7l6ykjont7dms2alunglmnlznlmjjwkyc67m5h325yd.onion"
    }
}
