// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

/** One block of a legal document; the files use only these Markdown basics. */
sealed interface LegalBlock {
    data class Title(val text: String) : LegalBlock
    data class Heading(val text: String) : LegalBlock
    data class Subheading(val text: String) : LegalBlock
    data class Paragraph(val text: String) : LegalBlock
    data class Bullets(val items: List<String>) : LegalBlock
    data class Numbered(val items: List<String>) : LegalBlock
}

/**
 * The RedVault Messages Terms of Service and Privacy Policy: legal/ at the
 * repository root (shared with the web and desktop apps), bundled as assets
 * by app/build.gradle.kts, so reading them never makes a network request.
 */
enum class LegalDocument(val assetName: String) {
    Terms("terms-of-service.md"),
    Privacy("privacy-policy.md");

    companion object {
        private val NUMBERED = Regex("^\\d+\\.\\s+(.*)$")

        /** "# " title, "## " and "### " headings, "- " and "1. " list items; any other line is a paragraph. Same as the web. */
        fun parse(markdown: String): List<LegalBlock> {
            val blocks = ArrayList<LegalBlock>()
            fun addItem(numbered: Boolean, item: String) {
                val last = blocks.lastOrNull()
                when {
                    numbered && last is LegalBlock.Numbered -> blocks[blocks.lastIndex] = LegalBlock.Numbered(last.items + item)
                    !numbered && last is LegalBlock.Bullets -> blocks[blocks.lastIndex] = LegalBlock.Bullets(last.items + item)
                    numbered -> blocks += LegalBlock.Numbered(listOf(item))
                    else -> blocks += LegalBlock.Bullets(listOf(item))
                }
            }
            for (raw in markdown.lines()) {
                val line = raw.trim()
                if (line.isEmpty()) continue
                val numbered = NUMBERED.find(line)
                when {
                    line.startsWith("### ") -> blocks += LegalBlock.Subheading(line.removePrefix("### "))
                    line.startsWith("## ") -> blocks += LegalBlock.Heading(line.removePrefix("## "))
                    line.startsWith("# ") -> blocks += LegalBlock.Title(line.removePrefix("# "))
                    line.startsWith("- ") -> addItem(false, line.removePrefix("- "))
                    numbered != null -> addItem(true, numbered.groupValues[1])
                    else -> blocks += LegalBlock.Paragraph(line)
                }
            }
            return blocks
        }
    }
}
