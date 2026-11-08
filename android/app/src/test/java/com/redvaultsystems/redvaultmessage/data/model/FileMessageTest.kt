// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FileMessageTest {
    // Exactly the shape ui/src/lib/fileMessage.ts serializes.
    private val webDescriptor = "\u0000rv-file:" + """{"type":"file","version":1,"objectId":"3F2504E0-4F89-11D3-9A0C-0305E82C3301",""" +
        """"fileKey":"Y2pxeH+GjZSboqmwt77FzNPa4ejv9v0ECxIZICcuNTw=","filename":"report.pdf","mimeType":"application/pdf",""" +
        """"originalSize":70000,"encryptedSize":70034,""" +
        """"ciphertextSha256":"b22cc5359bf22587e30e474b37a248c166664dc7b251eec50d0dfedeefbc0e19",""" +
        """"encryption":{"algorithm":"xchacha20poly1305-secretstream","version":1,"header":"AAECAwQFBgcICQoLDA0ODxAREhMUFRYX","chunkSize":65536}}"""

    @Test
    fun parsesTheWebFormat() {
        val d = FileMessage.parse(webDescriptor)
        assertNotNull(d)
        d!!
        assertEquals("3f2504e0-4f89-11d3-9a0c-0305e82c3301", d.objectId)
        assertEquals("report.pdf", d.filename)
        assertEquals(70_034L, d.encryptedSize)
    }

    @Test
    fun serializeThenParseRoundTrips() {
        val d = FileMessage.parse(webDescriptor)!!
        assertEquals(d, FileMessage.parse(FileMessage.serialize(d)))
        assertTrue(FileMessage.serialize(d).startsWith("\u0000rv-file:{\"type\":\"file\",\"version\":1"))
    }

    @Test
    fun rejectsMalformedDescriptors() {
        assertNull(FileMessage.parse("hello"))
        assertNull(FileMessage.parse("\u0000rv-file:not json"))
        assertNull(FileMessage.parse(webDescriptor.replace("\"chunkSize\":65536", "\"chunkSize\":1024")))
        assertNull(FileMessage.parse(webDescriptor.replace("3F2504E0", "../../etc")))
        assertNull(FileMessage.parse(webDescriptor.replace("\"originalSize\":70000", "\"originalSize\":-1")))
        assertNull(FileMessage.parse(webDescriptor.replace("\"version\":1,\"objectId\"", "\"version\":\"1\",\"objectId\"")))
    }

    @Test
    fun unknownMimeTypesFallBackToOpaqueBytes() {
        val d = FileMessage.parse(webDescriptor.replace("application/pdf", "text/html<script>"))!!
        assertEquals("application/octet-stream", d.mimeType)
    }

    @Test
    fun sanitizesFilenames() {
        assertEquals("passwd", FileMessage.sanitizeFilename("../../etc/passwd"))
        assertEquals("invoicegpj.exe", FileMessage.sanitizeFilename("invoice‮gpj.exe"))
        assertEquals("bashrc", FileMessage.sanitizeFilename(".bashrc"))
        assertEquals("_CON.txt", FileMessage.sanitizeFilename("CON.txt"))
        assertEquals("_CONIN$", FileMessage.sanitizeFilename("CONIN$"))
        assertEquals("_conout$.txt", FileMessage.sanitizeFilename("conout$.txt"))
        assertEquals("_COM¹", FileMessage.sanitizeFilename("COM¹"))
        assertEquals("_com².txt", FileMessage.sanitizeFilename("com².txt"))
        assertEquals("_LPT³.log", FileMessage.sanitizeFilename("LPT³.log"))
        assertEquals("_NUL .txt", FileMessage.sanitizeFilename("NUL .txt"))
        // Near misses stay as they are.
        assertEquals("COM10.txt", FileMessage.sanitizeFilename("COM10.txt"))
        assertEquals("CONSOLE.txt", FileMessage.sanitizeFilename("CONSOLE.txt"))
        assertEquals("CONIN.txt", FileMessage.sanitizeFilename("CONIN.txt"))
        assertEquals("a_b_.txt", FileMessage.sanitizeFilename("a<b>.txt"))
        assertEquals("file", FileMessage.sanitizeFilename("..."))
        val long = FileMessage.sanitizeFilename("x".repeat(300) + ".pdf")
        assertEquals(150, long.length)
        assertTrue(long.endsWith(".pdf"))
    }
}
