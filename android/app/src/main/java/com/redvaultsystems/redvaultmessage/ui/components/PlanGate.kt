// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.components

import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/**
 * One place for "this is part of a paid plan", like the desktop's PlanGate.qml.
 * A screen that finds the plan doesn't include a feature calls
 * PlanGate.ask(tier, feature); SignedInApp shows [UpgradeSheet], whose
 * "See plans" opens the Plans screen. Paid buttons show a lock rather than
 * looking available and refusing after a tap.
 */
object PlanGate {
    data class Request(val tier: String, val feature: String)

    var request by mutableStateOf<Request?>(null)
        private set

    /** [tier]: "pro" or "premium"; [feature]: what was tapped ("Sending files", "Screen sharing"). */
    fun ask(tier: String, feature: String) {
        request = Request(if (tier == "premium") "premium" else "pro", feature)
    }

    fun dismiss() {
        request = null
    }

    fun tierName(tier: String) = if (tier == "premium") "Premium" else "Pro"

    /** Whether [tier] includes what [needed] does ("pro" includes Pro, "premium" includes both). */
    fun includes(tier: String, needed: String): Boolean = when (needed) {
        "premium" -> tier == "premium"
        "pro" -> tier == "pro" || tier == "premium"
        else -> true
    }
}

/** "<Feature> is part of Pro/Premium", what that plan adds, "See plans" and "Not now". */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun UpgradeSheet(onSeePlans: () -> Unit) {
    val request = PlanGate.request ?: return
    val colors = RedvaultTheme.colors
    val adds = if (request.tier == "premium") {
        listOf(
            "Video call links",
            "Start group voice and video calls",
            "Screen sharing in calls",
            "Create and post in your own channels",
            "100 GB of Files storage",
            "Everything in Pro",
        )
    } else {
        listOf("Send files, photos and voice messages in chats", "Voice call links", "50 GB of Files storage")
    }
    ModalBottomSheet(
        onDismissRequest = PlanGate::dismiss,
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        containerColor = colors.surface,
    ) {
        Column(
            Modifier.fillMaxWidth().navigationBarsPadding().padding(horizontal = 20.dp).padding(bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Box(Modifier.size(44.dp).background(colors.lockBg, CircleShape), contentAlignment = Alignment.Center) {
                DsIcon(R.drawable.ui_lock, colors.lockText, 20.dp)
            }
            Text(
                "${request.feature} is part of ${PlanGate.tierName(request.tier)}",
                fontFamily = RedvaultFonts.display,
                fontSize = 22.sp,
                fontWeight = FontWeight.Bold,
                color = colors.text,
            )
            Text("${PlanGate.tierName(request.tier)} adds:", color = colors.text2, fontSize = 14.sp)
            adds.forEach { line ->
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                    DsIcon(R.drawable.ui_check, colors.accent, 16.dp)
                    Text(line, color = colors.text, fontSize = 14.sp)
                }
            }
            PillButton(
                stringResource(R.string.ui_see_plans),
                onClick = {
                    PlanGate.dismiss()
                    onSeePlans()
                },
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
            )
            PillButton(stringResource(R.string.ui_not_now), onClick = PlanGate::dismiss, modifier = Modifier.fillMaxWidth(), style = PillStyle.Quiet, height = 44.dp)
        }
    }
}

/** A small lock badge for a paid icon button the plan doesn't include. */
@Composable
fun LockBadge(modifier: Modifier = Modifier) {
    val colors = RedvaultTheme.colors
    Box(modifier.size(16.dp).background(colors.lockBg, CircleShape), contentAlignment = Alignment.Center) {
        DsIcon(R.drawable.ui_lock, colors.lockText, 10.dp)
    }
}
