// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.files

import java.io.File
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement

/**
 * What the Files viewer (FileViewer.kt) shows inside the app, and the checks on
 * the decrypted bytes. The same rules as the desktop (desktop/app/src/file_preview.cpp):
 * pictures, text, CSV, JSON, audio, video and PDF; nothing that can run
 * (HTML, SVG, scripts) and nothing else.
 */
object FilePreview {
    const val MAX_TEXT_BYTES = 512L * 1024
    const val MAX_CSV_ROWS = 1000
    const val MAX_CSV_COLUMNS = 50

    private const val MIB = 1024L * 1024

    enum class Kind { Image, Text, Csv, Json, Audio, Video, Pdf }

    fun kindFor(name: String): Kind? = when (name.substringAfterLast('.', "").lowercase()) {
        "jpg", "jpeg", "png", "gif", "webp" -> Kind.Image
        "txt" -> Kind.Text
        "csv" -> Kind.Csv
        "json" -> Kind.Json
        "mp3", "wav", "flac" -> Kind.Audio
        "mp4", "mov" -> Kind.Video
        "pdf" -> Kind.Pdf
        else -> null
    }

    /** Largest file of this kind the viewer decrypts. */
    fun limitFor(kind: Kind): Long = when (kind) {
        Kind.Image -> 100 * MIB
        Kind.Text, Kind.Csv, Kind.Json -> MAX_TEXT_BYTES
        Kind.Audio -> 1024 * MIB
        Kind.Video -> 4096 * MIB
        Kind.Pdf -> 500 * MIB
    }

    /** Valid UTF-8 with no NUL bytes (a BOM is fine). */
    fun isPlainText(bytes: ByteArray): Boolean {
        if (bytes.any { it == 0.toByte() }) return false
        return try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes))
            true
        } catch (_: CharacterCodingException) {
            false
        }
    }

    data class Csv(val rows: List<List<String>>, val truncated: Boolean)

    /** RFC 4180 CSV: quoted cells, doubled quotes, CRLF or LF rows; capped at maxRows x maxColumns. */
    fun parseCsv(text: String, maxRows: Int = MAX_CSV_ROWS, maxColumns: Int = MAX_CSV_COLUMNS): Csv {
        val rows = mutableListOf<List<String>>()
        var row = mutableListOf<String>()
        val cell = StringBuilder()
        var quoted = false
        var cut = false
        fun endCell() {
            if (row.size < maxColumns) row.add(cell.toString()) else cut = true
            cell.setLength(0)
        }
        fun endRow() {
            endCell()
            if (rows.size < maxRows) rows.add(row) else cut = true
            row = mutableListOf()
        }
        var i = 0
        val n = text.length
        while (i < n) {
            val c = text[i]
            if (quoted) {
                if (c == '"') {
                    if (i + 1 < n && text[i + 1] == '"') {
                        cell.append('"')
                        i++
                    } else {
                        quoted = false
                    }
                } else {
                    cell.append(c)
                }
            } else if (c == '"' && cell.isEmpty()) {
                quoted = true
            } else if (c == ',') {
                endCell()
            } else if (c == '\r' || c == '\n') {
                if (c == '\r' && i + 1 < n && text[i + 1] == '\n') i++
                endRow()
                if (rows.size >= maxRows && i + 1 < n) {
                    cut = true
                    return Csv(rows, cut)
                }
            } else {
                cell.append(c)
            }
            i++
        }
        if (cell.isNotEmpty() || row.isNotEmpty() || quoted) endRow()
        return Csv(rows, cut)
    }

    /** A decrypted text-like file, ready for the viewer; `error` is set when it can't be shown. */
    data class TextPreview(val text: String = "", val csv: Csv? = null, val error: String? = null)

    private val prettyJson = Json { prettyPrint = true }

    fun readText(file: File, kind: Kind): TextPreview {
        if (file.length() > MAX_TEXT_BYTES) return TextPreview(error = "This file is too big to show here. Save it instead.")
        var bytes = file.readBytes()
        if (!isPlainText(bytes)) return TextPreview(error = "This file isn't plain text, so it can't be shown here. Save it instead.")
        if (bytes.size >= 3 && bytes[0] == 0xEF.toByte() && bytes[1] == 0xBB.toByte() && bytes[2] == 0xBF.toByte()) {
            bytes = bytes.copyOfRange(3, bytes.size)
        }
        val text = bytes.toString(Charsets.UTF_8)
        return when (kind) {
            Kind.Json -> TextPreview(
                text = try {
                    prettyJson.encodeToString(JsonElement.serializer(), Json.parseToJsonElement(text))
                } catch (_: Exception) {
                    text  // not valid JSON: shown as it is
                },
            )
            Kind.Csv -> TextPreview(text = text, csv = parseCsv(text))
            else -> TextPreview(text = text)
        }
    }

    /** The file starts with a PDF header ("%PDF-" in its first kilobyte, as readers allow). */
    fun looksLikePdf(file: File): Boolean = try {
        val head = ByteArray(1024)
        val read = file.inputStream().use { it.read(head) }
        read > 0 && String(head, 0, read, Charsets.ISO_8859_1).contains("%PDF-")
    } catch (_: Exception) {
        false
    }
}
