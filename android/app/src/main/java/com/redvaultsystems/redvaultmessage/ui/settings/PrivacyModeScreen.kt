// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.NoticeTone
import com.redvaultsystems.redvaultmessage.ui.components.PrimaryButton
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/**
 * One place that shows every privacy protection and turns on the ones that
 * are switches. Some are always on in this app, and some features simply
 * don't exist (so there's nothing to leak): those are listed as such.
 */
@Composable
fun PrivacyModeScreen(
    readReceipts: Boolean,
    hideContent: Boolean,
    appLockOn: Boolean,
    onEnable: () -> Unit,
    onSetUpAppLock: () -> Unit,
    onBack: () -> Unit,
    typingIndicators: Boolean = false,
    /** Null until the server's setting has loaded. */
    showOnlineStatus: Boolean? = false,
) {
    val colors = RedvaultTheme.colors
    val allOn = !readReceipts && hideContent && appLockOn && !typingIndicators && showOnlineStatus != true
    SubScreen(title = "Maximum privacy", onBack = onBack) {
        Text(
            "Everything this app can do to keep what you do private, in one place.",
            color = colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        if (allOn) Notice("Maximum privacy is on.", NoticeTone.Success)

        PrivacyItem("Hide read receipts", if (!readReceipts) "On" else "Off: people see when you've read their messages", !readReceipts)
        PrivacyItem("Block screenshots and hide in Recents", if (hideContent) "On" else "Off", hideContent)
        PrivacyItem("Auto-lock app with a PIN", if (appLockOn) "On" else "Off: needs a PIN", appLockOn)
        PrivacyItem("Generic notifications", "Always on: notifications never show who it's from or what it says", true)
        PrivacyItem("No analytics", "Always on: the app contains no analytics or tracking", true)
        PrivacyItem("Hide typing status", if (!typingIndicators) "On" else "Off: contacts see when you're typing", !typingIndicators)
        PrivacyItem("Hide online status", if (showOnlineStatus != true) "On" else "Off: contacts see when you're using the app", showOnlineStatus != true)
        PrivacyItem("No link previews", "Always on: links are never fetched to build a preview", true)

        if (!allOn) {
            PrimaryButton(text = "Turn on maximum privacy", enabled = true) {
                onEnable()
                if (!appLockOn) onSetUpAppLock()
            }
            if (!appLockOn) {
                Text(
                    "You'll be asked to choose a PIN for the app lock.",
                    color = colors.text3,
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}

@Composable
private fun PrivacyItem(title: String, detail: String, on: Boolean) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(12.dp)).padding(14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Icon(
            painterResource(if (on) R.drawable.ic_check else R.drawable.ic_circle),
            contentDescription = if (on) "On" else "Off",
            tint = if (on) colors.accent else colors.text3,
            modifier = Modifier.size(18.dp),
        )
        Column(Modifier.weight(1f)) {
            Text(title, color = colors.text, fontWeight = FontWeight.Medium)
            Text(detail, color = colors.text2, style = MaterialTheme.typography.bodySmall)
        }
    }
}
