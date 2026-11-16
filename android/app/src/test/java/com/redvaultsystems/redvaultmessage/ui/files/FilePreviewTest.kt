// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import com.redvaultsystems.redvaultmessage.ui.files.FilePreview.Kind
import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The Files viewer's rules, the same as the desktop's (desktop/tests/file_preview_test.cpp). */
class FilePreviewTest {
    private val dir: File = Files.createTempDirectory("preview").toFile().apply { deleteOnExit() }

    private fun write(name: String, bytes: ByteArray) = File(dir, name).apply { writeBytes(bytes) }

    @Test
    fun onlyTheListedTypesPreview() {
        listOf("Photo.JPG", "a.jpeg", "a.png", "a.gif", "a.webp").forEach { assertEquals(it, Kind.Image, FilePreview.kindFor(it)) }
        assertEquals(Kind.Text, FilePreview.kindFor("notes.txt"))
        assertEquals(Kind.Csv, FilePreview.kindFor("table.csv"))
        assertEquals(Kind.Json, FilePreview.kindFor("data.json"))
        listOf("a.mp3", "a.wav", "a.flac").forEach { assertEquals(it, Kind.Audio, FilePreview.kindFor(it)) }
        listOf("a.mp4", "a.MOV").forEach { assertEquals(it, Kind.Video, FilePreview.kindFor(it)) }
        assertEquals(Kind.Pdf, FilePreview.kindFor("paper.pdf"))
        listOf("page.html", "logo.svg", "app.js", "setup.exe", "report.docx", "archive.zip", "movie.mkv", "noext", "pdf")
            .forEach { assertNull(it, FilePreview.kindFor(it)) }
        assertEquals(FilePreview.MAX_TEXT_BYTES, FilePreview.limitFor(Kind.Text))
    }

    @Test
    fun textMustBeUtf8WithoutNul() {
        assertTrue(FilePreview.isPlainText("hello\nworld".toByteArray()))
        assertTrue(FilePreview.isPlainText("café".toByteArray()))
        assertFalse(FilePreview.isPlainText(byteArrayOf(0x61, 0, 0x62)))
        assertFalse(FilePreview.isPlainText(byteArrayOf(0xFF.toByte(), 0xFE.toByte(), 0x41)))
        assertFalse(FilePreview.isPlainText(byteArrayOf(0x62, 0xC3.toByte())))
    }

    @Test
    fun csvCellsQuotesAndLineEnds() {
        val csv = FilePreview.parseCsv("name,note\r\n\"Doe, Jane\",\"said \"\"hi\"\"\"\nx,\"two\nlines\"\n")
        assertFalse(csv.truncated)
        assertEquals(listOf(listOf("name", "note"), listOf("Doe, Jane", "said \"hi\""), listOf("x", "two\nlines")), csv.rows)
        assertEquals(listOf("1", "2"), FilePreview.parseCsv("a,b\n1,2").rows[1])
        val limited = FilePreview.parseCsv("1,2,3\n4,5,6\n7,8,9\n", maxRows = 2, maxColumns = 2)
        assertTrue(limited.truncated)
        assertEquals(listOf(listOf("1", "2"), listOf("4", "5")), limited.rows)
    }

    @Test
    fun readsTextJsonAndCsv() {
        assertEquals("hello", FilePreview.readText(write("a.txt", byteArrayOf(0xEF.toByte(), 0xBB.toByte(), 0xBF.toByte()) + "hello".toByteArray()), Kind.Text).text)
        val json = FilePreview.readText(write("a.json", """{"a":[1,2]}""".toByteArray()), Kind.Json)
        assertNull(json.error)
        assertTrue(json.text.contains("\n"))
        assertEquals("{not json", FilePreview.readText(write("b.json", "{not json".toByteArray()), Kind.Json).text)
        val csv = FilePreview.readText(write("a.csv", "x,y\n1,2\n".toByteArray()), Kind.Csv)
        assertEquals(listOf("1", "2"), csv.csv!!.rows[1])
        assertNotNull(FilePreview.readText(write("c.txt", byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0, 0)), Kind.Text).error)
        assertNotNull(FilePreview.readText(write("big.txt", ByteArray((FilePreview.MAX_TEXT_BYTES + 1).toInt()) { 'a'.code.toByte() }), Kind.Text).error)
    }

    @Test
    fun pdfsAreCheckedByTheirHeader() {
        assertTrue(FilePreview.looksLikePdf(write("a.pdf", "%PDF-1.7\n".toByteArray())))
        assertTrue(FilePreview.looksLikePdf(write("b.pdf", " ".repeat(100).toByteArray() + "%PDF-1.4".toByteArray())))
        assertFalse(FilePreview.looksLikePdf(write("c.pdf", "<html><script>alert(1)</script>".toByteArray())))
        assertFalse(FilePreview.looksLikePdf(File(dir, "missing.pdf")))
    }
}
