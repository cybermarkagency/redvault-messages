package com.redvaultsystems.redvaultmessage.data.model

import com.redvaultsystems.redvaultmessage.data.model.OpenSourceInfo.ReportContact
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class OpenSourceInfoTest {
    @Test
    fun noConfiguredUrlMeansNoLink() {
        assertNull(OpenSourceInfo.sourceCodeUrl("", "0.4.0"))
        assertNull(OpenSourceInfo.sourceCodeUrl("   ", "0.4.0"))
    }

    @Test
    fun onlyHttpsUrlsWithAHostBecomeLinks() {
        assertEquals("https://example.org/src", OpenSourceInfo.sourceCodeUrl("https://example.org/src", "0.4.0"))
        assertEquals("https://example.org/src", OpenSourceInfo.sourceCodeUrl("  https://example.org/src ", "0.4.0"))
        for (bad in listOf(
            "http://example.org/src",
            "ftp://example.org/src",
            "javascript:alert(1)",
            "example.org/src",
            "https://",
            "https:///path",
            "https://exa mple.org",
            "https://example.org/\"x",
            "https://example.org\\x",
        )) {
            assertFalse(bad, OpenSourceInfo.isValidSourceUrl(bad))
            assertNull(bad, OpenSourceInfo.sourceCodeUrl(bad, "0.4.0"))
        }
    }

    @Test
    fun theVersionPlaceholderIsFilledIn() {
        assertEquals(
            "https://example.org/archive/v0.4.0.tar.gz",
            OpenSourceInfo.sourceCodeUrl("https://example.org/archive/v{version}.tar.gz", "0.4.0"),
        )
    }

    @Test
    fun theGradleScriptAppliesTheSameUrlRules() {
        // Unit tests run from android/app.
        val script = File("build.gradle.kts").readText()
        assertTrue(script.contains("redvaultSourceCodeUrl must be an https:// URL"))
        assertTrue(script.contains("buildConfigField(\"String\", \"SOURCE_CODE_URL\""))
        assertTrue(script.contains("buildConfigField(\"String\", \"SOURCE_CONTACT\""))
    }

    @Test
    fun reportContactFallsBackToTheTermsOfService() {
        assertEquals(ReportContact.TermsOfService, OpenSourceInfo.reportContact(""))
        assertEquals(ReportContact.Email("source@example.org"), OpenSourceInfo.reportContact("source@example.org"))
        assertEquals(ReportContact.Email("source@example.org"), OpenSourceInfo.reportContact("mailto:source@example.org"))
        assertEquals(ReportContact.Web("https://example.org/report"), OpenSourceInfo.reportContact("https://example.org/report"))
        assertEquals(ReportContact.Text("http://example.org/report"), OpenSourceInfo.reportContact("http://example.org/report"))
        // The Terms of Service the fallback points to really has a contact address.
        assertTrue(File("../../legal/${LegalDocument.Terms.assetName}").readText().contains("## 27. Contact"))
    }

    @Test
    fun everyAssetTheScreenOpensIsBundled() {
        val assets = File("src/main/assets")
        for (asset in OpenSourceInfo.ALL_ASSETS) {
            val file = File(assets, asset.assetPath)
            assertTrue("missing ${file.path}", file.isFile && file.length() > 0)
        }
        // And every bundled licence file is listed.
        val listed = OpenSourceInfo.BUNDLED_LICENSES.map { it.assetPath.removePrefix("licenses/") }.toSet()
        assertEquals(File(assets, "licenses").list()!!.toSet(), listed)
        assertTrue(File(assets, OpenSourceInfo.LICENSE.assetPath).readText().contains("GNU AFFERO GENERAL PUBLIC LICENSE"))
        assertTrue(File(assets, OpenSourceInfo.LICENSE.assetPath).readText().contains("Version 3, 19 November 2007"))
    }

    @Test
    fun textBlocksKeepEveryLine() {
        val text = (1..250).joinToString("\r\n") { "line $it" }
        val blocks = OpenSourceInfo.textBlocks(text, 60)
        assertEquals(5, blocks.size)
        assertEquals(text.replace("\r\n", "\n"), blocks.joinToString("\n"))
        assertEquals(listOf(""), OpenSourceInfo.textBlocks(""))
    }

    @Test
    fun theRequiredWordingIsExact() {
        assertEquals("Open-Source Licenses and Source Code", OpenSourceInfo.SECTION_TITLE)
        assertEquals(
            "RedVault Messages is free software, licensed under the GNU Affero General Public License, version 3 only (AGPL-3.0-only). It comes with no warranty.",
            OpenSourceInfo.LICENSE_STATEMENT,
        )
        assertEquals("The public source code link will appear here once it is published.", OpenSourceInfo.NO_SOURCE_LINK_YET)
        assertEquals("RedVault Messages is not affiliated with or endorsed by Signal Messenger LLC.", OpenSourceInfo.SIGNAL_DISCLAIMER)
    }
}
