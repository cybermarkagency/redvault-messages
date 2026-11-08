// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.redvaultsystems.redvaultmessage.BuildConfig
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.model.LegalDocument
import com.redvaultsystems.redvaultmessage.data.model.LicenseAsset
import com.redvaultsystems.redvaultmessage.data.model.OpenSourceInfo
import com.redvaultsystems.redvaultmessage.data.model.OpenSourceInfo.ReportContact
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.SectionLabel
import com.redvaultsystems.redvaultmessage.ui.components.SettingsRow
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.ui.onboarding.LegalDocumentDialog
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Settings > Open-Source Licenses and Source Code: the AGPL-3.0-only notice, this version, the source
 * link (from the redvaultSourceCodeUrl Gradle property; none is shown until it is set), the full licence,
 * the third-party notices, the bundled font and icon licences, and where to report source problems.
 * Everything is read from the APK's assets, so nothing here makes a network request.
 */
@Composable
internal fun OpenSourceSection() {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val appLock = LocalAppLock.current
    var readingPath by rememberSaveable { mutableStateOf<String?>(null) }
    var readingTerms by rememberSaveable { mutableStateOf(false) }
    var error by rememberSaveable { mutableStateOf<String?>(null) }
    val sourceUrl = OpenSourceInfo.sourceCodeUrl(BuildConfig.SOURCE_CODE_URL, BuildConfig.VERSION_NAME)
    val contact = OpenSourceInfo.reportContact(BuildConfig.SOURCE_CONTACT)

    fun open(intent: Intent, fallback: String) {
        error = null
        try {
            appLock?.expectExternalReturn()
            context.startActivity(intent)
        } catch (_: ActivityNotFoundException) {
            error = fallback
        }
    }

    SectionLabel(OpenSourceInfo.SECTION_TITLE)
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(OpenSourceInfo.LICENSE_STATEMENT, color = colors.text2, fontSize = 14.sp, lineHeight = 21.sp)
        Text("Version ${BuildConfig.VERSION_NAME}", color = colors.text, fontFamily = RedvaultFonts.mono, fontSize = 13.sp)
        if (sourceUrl == null) Text(OpenSourceInfo.NO_SOURCE_LINK_YET, color = colors.text3, fontSize = 13.sp)
    }
    if (sourceUrl != null) {
        SettingsRow(
            title = OpenSourceInfo.SOURCE_LINK_LABEL,
            description = sourceUrl,
            onClick = { open(Intent(Intent.ACTION_VIEW, Uri.parse(sourceUrl)), "No browser found to open $sourceUrl") },
            icon = R.drawable.ui_external,
        )
    }
    SettingsRow(
        title = OpenSourceInfo.LICENSE.title,
        description = "The full licence text (AGPL-3.0-only)",
        onClick = { readingPath = OpenSourceInfo.LICENSE.assetPath },
        icon = R.drawable.ui_file,
    )
    SettingsRow(
        title = OpenSourceInfo.THIRD_PARTY_NOTICES.title,
        description = "The libraries this app includes and their licences",
        onClick = { readingPath = OpenSourceInfo.THIRD_PARTY_NOTICES.assetPath },
        icon = R.drawable.ui_file,
    )
    OpenSourceInfo.BUNDLED_LICENSES.forEach { asset ->
        SettingsRow(title = asset.title, description = "Bundled with the app", onClick = { readingPath = asset.assetPath }, icon = R.drawable.ui_file)
    }
    when (contact) {
        is ReportContact.Email -> SettingsRow(
            title = "Report a source code problem",
            description = "If the source for this version is missing or incomplete, write to ${contact.address}",
            onClick = { open(Intent(Intent.ACTION_SENDTO, Uri.parse("mailto:${contact.address}")), "No e-mail app found. Write to ${contact.address}") },
            icon = R.drawable.ui_info,
        )
        is ReportContact.Web -> SettingsRow(
            title = "Report a source code problem",
            description = "If the source for this version is missing or incomplete: ${contact.url}",
            onClick = { open(Intent(Intent.ACTION_VIEW, Uri.parse(contact.url)), "No browser found to open ${contact.url}") },
            icon = R.drawable.ui_info,
        )
        is ReportContact.Text -> SettingsRow(
            title = "Report a source code problem",
            description = "If the source for this version is missing or incomplete: ${contact.text}",
            icon = R.drawable.ui_info,
        )
        ReportContact.TermsOfService -> SettingsRow(
            title = "Report a source code problem",
            description = "If the source for this version is missing or incomplete, write to the contact address in the Terms of Service",
            onClick = { readingTerms = true },
            icon = R.drawable.ui_info,
        )
    }
    error?.let { Notice(it) }
    Text(OpenSourceInfo.SIGNAL_DISCLAIMER, color = colors.text3, fontSize = 12.5.sp)

    readingPath?.let { path ->
        OpenSourceInfo.ALL_ASSETS.firstOrNull { it.assetPath == path }?.let { LicenseTextDialog(it, onDismiss = { readingPath = null }) }
    }
    if (readingTerms) LegalDocumentDialog(LegalDocument.Terms, onDismiss = { readingTerms = false })
}

private sealed interface Loaded {
    data object Loading : Loaded
    data object Failed : Loaded
    data class Text(val blocks: List<String>) : Loaded
}

private fun readAsset(context: Context, path: String): String =
    context.assets.open(path).bufferedReader(Charsets.UTF_8).use { it.readText() }

/** A bundled plain-text licence file, full screen and scrollable; read and split off the main thread. */
@Composable
private fun LicenseTextDialog(asset: LicenseAsset, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val loaded by produceState<Loaded>(Loaded.Loading, asset) {
        value = withContext(Dispatchers.IO) {
            runCatching { Loaded.Text(OpenSourceInfo.textBlocks(readAsset(context, asset.assetPath))) }.getOrDefault(Loaded.Failed)
        }
    }

    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Column(Modifier.fillMaxSize().background(colors.bg).systemBarsPadding().padding(horizontal = 20.dp, vertical = 12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    asset.title,
                    fontFamily = RedvaultFonts.display,
                    fontSize = 22.sp,
                    fontWeight = FontWeight.Bold,
                    color = colors.text,
                    modifier = Modifier.weight(1f),
                )
                Box(
                    Modifier
                        .size(36.dp)
                        .clip(CircleShape)
                        .background(colors.surface2)
                        .clickable(role = Role.Button, onClick = onDismiss)
                        .semantics { contentDescription = "Close" },
                    contentAlignment = Alignment.Center,
                ) { Text("×", color = colors.text2, fontSize = 20.sp) }
            }
            Box(Modifier.weight(1f).fillMaxWidth().padding(top = 12.dp)) {
                when (val state = loaded) {
                    Loaded.Loading -> Text("Loading…", color = colors.text3, fontSize = 14.sp)
                    Loaded.Failed -> Text("Couldn't open ${asset.assetPath}.", color = colors.text3, fontSize = 14.sp)
                    is Loaded.Text -> LazyColumn(Modifier.fillMaxSize()) {
                        items(state.blocks) { block ->
                            Text(block, color = colors.text2, fontFamily = RedvaultFonts.mono, fontSize = 11.5.sp, lineHeight = 16.sp)
                        }
                    }
                }
            }
            PillButton("Done", onDismiss, Modifier.fillMaxWidth().padding(top = 8.dp).clip(RoundedCornerShape(50)))
        }
    }
}
