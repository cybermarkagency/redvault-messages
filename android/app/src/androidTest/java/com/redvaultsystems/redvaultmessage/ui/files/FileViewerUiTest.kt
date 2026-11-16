// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import android.graphics.Bitmap
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import java.io.File
import org.junit.Assume.assumeTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * The Files viewer on a device, with real files of each kind: Android's own
 * decoders, player and PDF renderer. The files come from a sample folder in
 * the app's private storage, put there by the person running it (debug build):
 *   adb push <folder>/. /data/local/tmp/preview-samples/
 *   then, for each file: adb shell run-as com.redvaultsystems.redvaultmessage cp /data/local/tmp/preview-samples/<name> cache/preview-samples/<name>
 *   (after: adb shell run-as com.redvaultsystems.redvaultmessage mkdir -p cache/preview-samples)
 * A test whose sample isn't there is skipped. Screenshots go to cache/preview-shots
 * (adb exec-out run-as com.redvaultsystems.redvaultmessage cat cache/preview-shots/<name>.png).
 */
@RunWith(AndroidJUnit4::class)
class FileViewerUiTest {
    @get:Rule val rule = createComposeRule()

    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private val samples = File(context.cacheDir, "preview-samples")

    private fun sample(name: String): File {
        val file = File(samples, name)
        assumeTrue("put $name into ${samples.path}", file.isFile)
        return file
    }

    private class Shown(val name: String, val kind: FilePreview.Kind, val loaded: Loaded)
    private var shown by mutableStateOf<Shown?>(null)
    private var started = false

    /** The screen is set once per test; later files replace what it shows. */
    private fun show(name: String, kind: FilePreview.Kind, loaded: Loaded) {
        shown = Shown(name, kind, loaded)
        if (!started) {
            started = true
            rule.setContent {
                RedvaultTheme {
                    shown?.let { s -> key(s) { FileViewerFrame(s.name, 1234, s.kind, s.loaded, 1f, onDismiss = {}, onSave = {}) } }
                }
            }
        }
        rule.waitForIdle()
    }

    private fun shoot(name: String) {
        Thread.sleep(1500)
        rule.waitForIdle()
        val out = File(context.cacheDir, "preview-shots").apply { mkdirs() }
        File(out, "$name.png").outputStream().use { rule.onRoot().captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    @Test
    fun pictures() {
        for (name in listOf("photo.png", "photo.jpg", "photo.webp", "anim.gif")) {
            show(name, FilePreview.Kind.Image, Loaded.Ready(sample(name), null))
            rule.waitUntil(10_000) { rule.onAllNodes(androidx.compose.ui.test.hasContentDescription("Preview of $name")).fetchSemanticsNodes().isNotEmpty() }
            shoot(name.replace('.', '_'))
        }
    }

    @Test
    fun textJsonAndCsv() {
        val txt = sample("notes.txt")
        show("notes.txt", FilePreview.Kind.Text, Loaded.Ready(null, FilePreview.readText(txt, FilePreview.Kind.Text)))
        rule.onNodeWithText("<b>not bold</b>", substring = true).assertExists()
        shoot("notes_txt")

        val json = sample("data.json")
        show("data.json", FilePreview.Kind.Json, Loaded.Ready(null, FilePreview.readText(json, FilePreview.Kind.Json)))
        rule.onNodeWithText("\"preview\": true", substring = true).assertExists()
        shoot("data_json")

        val csv = sample("table.csv")
        show("table.csv", FilePreview.Kind.Csv, Loaded.Ready(null, FilePreview.readText(csv, FilePreview.Kind.Csv)))
        rule.onNodeWithText("Doe, Jane").assertExists()
        rule.onNodeWithText("1,200").assertExists()
        shoot("table_csv")
    }

    @Test
    fun audio() {
        for (name in listOf("song.mp3", "beep.wav", "tone.flac")) {
            show(name, FilePreview.Kind.Audio, Loaded.Ready(sample(name), null))
            Thread.sleep(1500)
            rule.onNodeWithText("can't be played", substring = true).assertDoesNotExist()
            shoot(name.replace('.', '_'))
        }
    }

    @Test
    fun video() {
        for (name in listOf("clip.mp4", "clip.mov")) {
            show(name, FilePreview.Kind.Video, Loaded.Ready(sample(name), null))
            Thread.sleep(2500)
            rule.onNodeWithText("can't be played", substring = true).assertDoesNotExist()
            shoot(name.replace('.', '_'))
        }
    }

    @Test
    fun pdf() {
        show("paper.pdf", FilePreview.Kind.Pdf, Loaded.Ready(sample("paper.pdf"), null))
        rule.waitUntil(10_000) { rule.onAllNodes(androidx.compose.ui.test.hasContentDescription("Page 1 of 2")).fetchSemanticsNodes().isNotEmpty() }
        shoot("paper_pdf")
    }

    @Test
    fun failureOffersSave() {
        show("huge.mp4", FilePreview.Kind.Video, Loaded.Failed("This file is too big to preview here."))
        rule.onNodeWithText("This file is too big to preview here.").assertExists()
        rule.onNodeWithText("Save instead").assertExists()
        shoot("error")
    }
}
