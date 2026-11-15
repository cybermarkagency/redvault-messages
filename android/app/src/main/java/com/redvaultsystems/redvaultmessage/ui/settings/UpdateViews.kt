// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.redvaultsystems.redvaultmessage.BuildConfig
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.SectionLabel
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import com.redvaultsystems.redvaultmessage.update.AppUpdates
import com.redvaultsystems.redvaultmessage.update.AppUpdates.State

/**
 * The strip above the tabs when a newer version is out (update/AppUpdates.kt): "Update" downloads it through Tor,
 * checks it against the signed manifest and hands it to Android's installer. Nothing shows while there's nothing to do.
 */
@Composable
fun UpdateBanner(updates: AppUpdates?) {
    if (updates == null) return
    val state by updates.state.collectAsStateWithLifecycle()
    val dismissed by updates.dismissedVersionCode.collectAsStateWithLifecycle()
    val release = when (val s = state) {
        is State.Available -> s.release
        is State.Downloading -> s.release
        is State.NeedsPermission -> s.release
        is State.Installing -> s.release
        is State.Failed -> s.release
        else -> null
    } ?: return
    if (state is State.Available && release.versionCode <= dismissed) return
    val colors = RedvaultTheme.colors
    Column(
        Modifier.fillMaxWidth().background(colors.accentSoft).padding(horizontal = 16.dp, vertical = 10.dp)
            .semantics { liveRegion = LiveRegionMode.Polite },
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        UpdateBody(updates, state, release.version, compact = true)
    }
}

/** Settings › About: this version, the last check, and "Check for updates". */
@Composable
fun UpdateSection(updates: AppUpdates?) {
    if (updates == null) return
    val state by updates.state.collectAsStateWithLifecycle()
    val colors = RedvaultTheme.colors
    SectionLabel("Updates")
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(16.dp)
            .semantics { liveRegion = LiveRegionMode.Polite },
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("RedVaultMessages ${BuildConfig.VERSION_NAME}", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
        when (val s = state) {
            State.Idle -> Text("Updates are checked through Tor when the app starts, then once a day.", color = colors.text2, fontSize = 13.sp)
            State.Checking -> Text("Checking for updates…", color = colors.text2, fontSize = 13.sp)
            is State.UpToDate -> Text(
                if (s.stale) "No newer version found, but the update information is out of date: check the website if this lasts."
                else "You have the latest version.",
                color = if (s.stale) colors.warn else colors.text2, fontSize = 13.sp,
            )
            else -> {
                val version = when (s) {
                    is State.Available -> s.release.version
                    is State.Downloading -> s.release.version
                    is State.NeedsPermission -> s.release.version
                    is State.Installing -> s.release.version
                    is State.Failed -> s.release?.version
                    else -> null
                }
                if (version != null) UpdateBody(updates, s, version, compact = false)
                else if (s is State.Failed) Text(s.message, color = colors.danger, fontSize = 13.sp)
            }
        }
        if (state !is State.Checking && state !is State.Downloading && state !is State.Installing) {
            PillButton("Check for updates", { updates.checkNow() }, Modifier.fillMaxWidth(), style = PillStyle.Secondary, icon = R.drawable.ui_refresh)
        }
    }
}

@Composable
private fun UpdateBody(updates: AppUpdates, state: State, version: String, compact: Boolean) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    when (state) {
        is State.Available -> {
            Text("RedVaultMessages $version is available.", color = colors.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
            if (!compact && state.release.notes.isNotBlank()) Text(state.release.notes, color = colors.text2, fontSize = 13.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                PillButton("Update", { updates.update(state.release) }, Modifier.weight(1f), height = 48.dp, icon = R.drawable.ui_download)
                if (compact) PillButton("Later", { updates.dismiss(state.release) }, Modifier.weight(1f), style = PillStyle.Secondary, height = 48.dp)
            }
        }
        is State.Downloading -> {
            Text("Downloading $version through Tor… ${state.percent}%", color = colors.text, fontSize = 14.sp)
            LinearProgressIndicator(progress = { state.percent / 100f }, modifier = Modifier.fillMaxWidth(), color = colors.accentFill, trackColor = colors.surface3)
        }
        is State.NeedsPermission -> {
            Text(
                "$version is downloaded and checked. To install it, allow RedVaultMessages to install updates, then come back and tap Install.",
                color = colors.text, fontSize = 14.sp,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PillButton("Allow", { context.startActivity(updates.installPermissionIntent()) }, Modifier.weight(1f), style = PillStyle.Secondary, height = 48.dp)
                PillButton("Install", { updates.continueInstall(state.release) }, Modifier.weight(1f), height = 48.dp)
            }
        }
        is State.Installing -> Text("Installing $version: confirm on the screen Android shows.", color = colors.text, fontSize = 14.sp)
        is State.Failed -> {
            Text(state.message, color = colors.danger, fontSize = 13.sp)
            state.release?.let { release ->
                PillButton("Try again", { updates.update(release) }, Modifier.fillMaxWidth(), style = PillStyle.Secondary, height = 48.dp)
            }
        }
        else -> Unit
    }
}
