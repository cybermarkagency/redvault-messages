// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.api.TOR_UNAVAILABLE_MESSAGE
import com.redvaultsystems.redvaultmessage.tor.TorController
import com.redvaultsystems.redvaultmessage.tor.TorState
import com.redvaultsystems.redvaultmessage.tor.TorStatus
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.components.SectionCaps
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/**
 * Settings › Privacy & Security › Network routing (docs/tor-by-default.md rule 7): how traffic leaves the phone,
 * read-only. There is no switch: everything but calls goes through Tor, and nothing is ever sent another way.
 */
@Composable
fun NetworkRoutingScreen(
    tor: TorController?,
    queued: Int,
    onBack: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    val status: TorStatus? = tor?.status?.collectAsStateWithLifecycle()?.value
    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(start = 8.dp, end = 12.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            RoundButton(R.drawable.ui_chevron_left, "Back", onClick = onBack, container = Color.Transparent, iconSize = 24.dp)
        }
        Column(
            Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Text("Network routing", fontFamily = RedvaultFonts.display, fontSize = 30.sp, fontWeight = FontWeight.Bold, color = colors.text)

            if (tor == null) {
                // Only a debug build pointed at a local test server.
                Callout(
                    "Development server: this test build talks to it directly, without Tor. Calls go through the call relay (TURN).",
                    CalloutTone.Neutral, R.drawable.ui_info,
                )
            }

            Card {
                Line(
                    "Messages, files and account",
                    "Text, photos, files, voice messages, Stories, avatars and notifications",
                    if (tor != null) "Through Tor" else "Direct (test server)",
                    if (tor != null) colors.accent else colors.text2,
                )
                Divider()
                Line("Calls", "Voice and video, end-to-end encrypted", "Encrypted relay (not Tor)", colors.text2)
            }

            if (tor != null) {
                SectionCaps("Tor", edge = 0.dp)
                Card {
                    val (label, tone) = torLabel(status)
                    Line("Tor connection", status?.detail?.takeIf { it.isNotBlank() } ?: "Built into this app", label, tone)
                    if (queued > 0) {
                        Divider()
                        Line("Waiting to send", "Encrypted on this phone until Tor connects", "$queued queued", colors.text3)
                    }
                }
                if (status?.state == TorState.Unavailable) Callout(TOR_UNAVAILABLE_MESSAGE, CalloutTone.Warn, R.drawable.ui_alert_circle)
                Text(
                    "Messages, files and your account reach the service only through Tor, at its onion address. There's no setting that sends " +
                        "them another way: if Tor can't connect, they wait here, encrypted, and go out once it does. They're never shown as sent " +
                        "before the server has them. Notifications come from the app's own background connection, through Tor too: no push " +
                        "service run by anyone else is used.",
                    color = colors.text2, fontSize = 14.sp,
                )
            }
            Text(
                "Calls don't go through Tor: Tor can't carry live audio and video well. They're relayed through the call server's TURN relay, " +
                    "so the people you call never learn your IP address, but the relay itself does while the call lasts. Calls are end-to-end encrypted " +
                    "either way; they aren't anonymous. Setting up a call still goes through Tor.",
                color = colors.text2, fontSize = 14.sp,
            )
            Text(
                "Tor hides your IP address from the message server and the network from what you send; it doesn't make you anonymous by itself. " +
                    "What you write, who you write to, and the device you use can still identify you. Links you open go to your browser, outside the app.",
                color = colors.text3, fontSize = 13.sp,
            )
            Spacer(Modifier.height(16.dp))
        }
    }
}

/** "Connecting through Tor…" / "Can't reach Tor" (docs/tor-by-default.md rule 4), the same words everywhere. */
@Composable
private fun torLabel(status: TorStatus?): Pair<String, Color> {
    val colors = RedvaultTheme.colors
    return when (status?.state) {
        TorState.Connected -> "Connected" to colors.accent
        TorState.Connecting -> "Connecting through Tor… ${status.progress}%" to colors.warn
        else -> "Can't reach Tor" to colors.danger
    }
}

@Composable
private fun Card(content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(RedvaultTheme.colors.surface), content = content)
}

@Composable
private fun Divider() = Box(Modifier.fillMaxWidth().height(1.dp).background(RedvaultTheme.colors.border))

@Composable
private fun Line(title: String, detail: String, value: String, tone: Color) {
    val colors = RedvaultTheme.colors
    Row(Modifier.fillMaxWidth().padding(14.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, color = colors.text, fontSize = 15.sp)
            Text(detail, color = colors.text3, fontSize = 12.5.sp)
        }
        if (value.isNotEmpty()) Text(value, color = tone, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
    }
}

/**
 * The thin strip above the tabs (whenever the app goes through Tor): connecting / connected / can't reach Tor, what's
 * waiting, and, during a call, that it goes WebRTC through the TURN relay rather than Tor.
 */
@Composable
fun TorStatusStrip(status: TorStatus, queued: Int, pausedUploads: Int, callActive: Boolean) {
    val colors = RedvaultTheme.colors
    val (text, tone) = when (status.state) {
        TorState.Connected -> "Tor connected" to colors.accent
        TorState.Connecting -> "Connecting through Tor… ${status.progress}%" to colors.warn
        TorState.Unavailable -> "Can't reach Tor" to colors.danger
    }
    val extras = buildList {
        if (queued > 0) add("$queued queued")
        if (pausedUploads > 0) add("$pausedUploads upload${if (pausedUploads == 1) "" else "s"} paused")
        if (callActive) add("call via encrypted relay, not Tor")
    }
    Row(
        Modifier.fillMaxWidth().background(colors.surface).padding(horizontal = 16.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Box(Modifier.size(8.dp).clip(RoundedCornerShape(4.dp)).background(tone))
        Text(text, color = tone, fontSize = 12.5.sp, fontWeight = FontWeight.SemiBold)
        if (extras.isNotEmpty()) Text(extras.joinToString(" · "), color = colors.text3, fontSize = 12.5.sp, maxLines = 1)
    }
}
