package com.redvaultsystems.redvaultmessage.data.model

import java.net.URI

/**
 * A plain-text licence file bundled as an asset, shown by Settings > Open-Source Licenses and Source Code.
 * [assetPath] is relative to the APK's assets/ folder (app/src/main/assets in the source).
 */
data class LicenseAsset(val title: String, val assetPath: String)

/**
 * What the Open-Source Licenses and Source Code section shows. The source link and the report contact
 * come from the Gradle properties redvaultSourceCodeUrl and redvaultSourceContact (BuildConfig
 * SOURCE_CODE_URL / SOURCE_CONTACT, see android/README.md); app/build.gradle.kts already refuses a
 * non-https URL, and [sourceCodeUrl] checks again so a bad value is never shown as a link.
 */
object OpenSourceInfo {
    const val SECTION_TITLE = "Open-Source Licenses and Source Code"
    const val LICENSE_STATEMENT =
        "RedVault Messages is free software, licensed under the GNU Affero General Public License, version 3 only " +
            "(AGPL-3.0-only). It comes with no warranty."
    const val NO_SOURCE_LINK_YET = "The public source code link will appear here once it is published."
    const val SOURCE_LINK_LABEL = "Source code for this version"
    const val SIGNAL_DISCLAIMER = "RedVault Messages is not affiliated with or endorsed by Signal Messenger LLC."

    /** Optional placeholder in redvaultSourceCodeUrl, replaced with the app's version name. */
    const val VERSION_PLACEHOLDER = "{version}"

    val LICENSE = LicenseAsset("GNU Affero General Public License v3", "legal/LICENSE.txt")
    val THIRD_PARTY_NOTICES = LicenseAsset("Third-party notices", "legal/THIRD_PARTY_NOTICES.txt")

    /** The licences of the fonts and icons bundled with the app (assets/licenses). */
    val BUNDLED_LICENSES = listOf(
        LicenseAsset("Bricolage Grotesque (SIL Open Font License 1.1)", "licenses/bricolage-grotesque-OFL.txt"),
        LicenseAsset("IBM Plex Sans (SIL Open Font License 1.1)", "licenses/ibm-plex-sans-OFL.txt"),
        LicenseAsset("IBM Plex Mono (SIL Open Font License 1.1)", "licenses/ibm-plex-mono-OFL.txt"),
        LicenseAsset("Lucide icons (ISC License)", "licenses/lucide-LICENSE.txt"),
    )

    /** Every asset the section can open. */
    val ALL_ASSETS: List<LicenseAsset> get() = listOf(LICENSE, THIRD_PARTY_NOTICES) + BUNDLED_LICENSES

    /** True for an absolute https URL with a host and nothing that would break it (spaces, quotes, backslashes). */
    fun isValidSourceUrl(url: String): Boolean {
        if (url.isEmpty() || url.any { it.isWhitespace() || it == '"' || it == '\\' }) return false
        val uri = runCatching { URI(url.replace(VERSION_PLACEHOLDER, "0")) }.getOrNull() ?: return false
        return uri.scheme.equals("https", ignoreCase = true) && !uri.host.isNullOrEmpty()
    }

    /** The link for this version, or null when no (valid) URL is configured: then no link is shown at all. */
    fun sourceCodeUrl(configured: String, versionName: String): String? {
        val url = configured.trim()
        if (!isValidSourceUrl(url)) return null
        return url.replace(VERSION_PLACEHOLDER, versionName)
    }

    /** How a source-availability problem is reported. */
    sealed interface ReportContact {
        /** A configured e-mail address: opened with mailto:. */
        data class Email(val address: String) : ReportContact
        /** A configured https page. */
        data class Web(val url: String) : ReportContact
        /** Configured, but neither an address nor an https URL: shown as text only. */
        data class Text(val text: String) : ReportContact
        /** Nothing configured: the contact address in the Terms of Service. */
        data object TermsOfService : ReportContact
    }

    /**
     * Splits a long text into blocks of at most [linesPerBlock] lines, so the viewer's lazy list only lays
     * out what is on screen (the third-party notices are several megabytes). Joining the blocks with
     * "\n" gives the text back (line endings normalised to "\n").
     */
    fun textBlocks(text: String, linesPerBlock: Int = 60): List<String> {
        require(linesPerBlock > 0)
        return text.replace("\r\n", "\n").split('\n').chunked(linesPerBlock) { it.joinToString("\n") }
    }

    private val EMAIL =Regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$")

    fun reportContact(configured: String): ReportContact {
        val value = configured.trim()
        return when {
            value.isEmpty() -> ReportContact.TermsOfService
            value.startsWith("mailto:", ignoreCase = true) && EMAIL.matches(value.substring(7)) -> ReportContact.Email(value.substring(7))
            EMAIL.matches(value) -> ReportContact.Email(value)
            isValidSourceUrl(value) -> ReportContact.Web(value)
            else -> ReportContact.Text(value)
        }
    }
}
