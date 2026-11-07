package com.redvaultsystems.redvaultmessage.data.model

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class LegalDocumentTest {
    @Test
    fun parsesATitleHeadingsParagraphsAndLists() {
        // The same case as ui/src/test/signUpConsent.test.tsx.
        assertEquals(
            listOf(
                LegalBlock.Title("T"),
                LegalBlock.Paragraph("Intro"),
                LegalBlock.Heading("1. A"),
                LegalBlock.Bullets(listOf("x;", "y")),
                LegalBlock.Subheading("Sub"),
                LegalBlock.Numbered(listOf("one", "two")),
                LegalBlock.Paragraph("End"),
            ),
            LegalDocument.parse("# T\n\nIntro\n\n## 1. A\n\n- x;\n- y\n\n### Sub\n\n1. one\n2. two\n\nEnd"),
        )
    }

    @Test
    fun theBundledDocumentsHaveEverySection() {
        // Unit tests run from android/app; the assets come from legal/ at the repository root.
        fun read(document: LegalDocument) = File("../../legal/${document.assetName}").readText()
        assertEquals(27, LegalDocument.parse(read(LegalDocument.Terms)).count { it is LegalBlock.Heading })
        assertEquals(26, LegalDocument.parse(read(LegalDocument.Privacy)).count { it is LegalBlock.Heading })
        assertTrue(read(LegalDocument.Terms).contains("Effective Date: 6 October 2026"))
    }
}
