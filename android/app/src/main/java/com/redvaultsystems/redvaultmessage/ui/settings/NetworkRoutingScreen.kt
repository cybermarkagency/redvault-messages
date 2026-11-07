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
import com.redvaultsystems.redvaultmessage.ui.components.RvSwitch
import com.redvaultsystems.redvaultmessage.ui.components.SectionCaps
import com.redvaultsystems.redvaultmessage.ui.files.MonoFamily
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/**
 * Settings › Privacy › Network routing (docs/tor-routing-plan.md): how each kind
 * of traffic leaves the phone. Read-only on purpose: there is no switch that
 * sends Tor-only features directly. The one choice here is Android's strict
 * no-push mode.
 */
@Composable
fun NetworkRoutingScreen(
    tor: TorController?,
    queued: Int,
    strictNoPush: Boolean,
    onStrictNoPushChange: (Boolean) -> Unit,
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
                Callout(
                    "This server isn't on Tor routing yet: everything goes over HTTPS to the message relay, which sees your IP address. Calls go through the call relay (TURN).",
                    CalloutTone.Neutral, R.drawable.ui_info,
                )
            } else {
                SectionCaps("Tor", edge = 0.dp)
                Card {
                    val (label, tone) = when (status?.state) {
                        TorState.Connected -> "Connected" to colors.accent
                        TorState.Connecting -> "Connecting · ${status.progress}%" to colors.warn
                        else -> "Unavailable" to colors.danger
                    }
                    Line("Tor", status?.detail?.takeIf { it.isNotBlank() } ?: "Built into this app", label, tone)
                    Divider()
                    Line("Onion service", tor.onionHost.take(16) + "…" + tor.onionHost.takeLast(12), if (queued > 0) "$queued queued" else "", colors.text3)
                }
                if (status?.state == TorState.Unavailable) Callout(TOR_UNAVAILABLE_MESSAGE, CalloutTone.Warn, R.drawable.ui_alert_circle)
            }

            SectionCaps("How each kind of traffic travels", edge = 0.dp)
            Card {
                val route = if (tor != null) "Tor only" else "HTTPS"
                listOf("Text messages", "Encrypted photos", "Encrypted files", "Private Stories", "Voice messages").forEachIndexed { i, feature ->
                    if (i > 0) Divider()
                    Line(feature, "End-to-end encrypted", route, if (tor != null) colors.accent else colors.text2)
                }
                Divider()
                Line("Voice and video calls", "End-to-end encrypted media", "WebRTC + TURN", colors.text2)
            }
            if (tor != null) {
                Text(
                    "Messages, photos, files, Stories and voice messages leave this phone only through Tor, to the service's onion address. " +
                        "There's no setting that sends them another way: if Tor can't connect, they wait here, encrypted, and go out once it does. " +
                        "They're never shown as sent before the server has them.",
                    color = colors.text2, fontSize = 14.sp,
                )
            }
            Text(
                "Calls don't go through Tor: Tor can't carry live audio and video well. They're relayed through the call server's TURN relay, " +
                    "so the people you call never learn your IP address, but the relay itself does while the call lasts. Calls are end-to-end encrypted " +
                    "either way; they aren't anonymous.",
                color = colors.text2, fontSize = 14.sp,
            )
            Text(
                "Tor hides your IP address from the message server and the network from what you send; it doesn't make you anonymous by itself. " +
                    "What you write, who you write to, and the device you use can still identify you.",
                color = colors.text3, fontSize = 13.sp,
            )

            run {
                SectionCaps("Notifications", edge = 0.dp)
                Card {
                    Row(Modifier.fillMaxWidth().padding(14.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text("Strict no-push mode", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                            Text(
                                if (tor != null) {
                                    "Never a push service run by someone else. Notifications come only from this server, through Tor."
                                } else {
                                    "No push registration and no background connection: nothing can wake this phone for Redvault."
                                },
                                color = colors.text3, fontSize = 12.5.sp,
                            )
                        }
                        RvSwitch(checked = strictNoPush, onCheckedChange = onStrictNoPushChange, description = "Strict no-push mode")
                    }
                }
                Text(
                    if (strictNoPush && tor != null) {
                        "On: no UnifiedPush app is used. Notifications come only through Redvault's own background connection to this " +
                            "server, inside Tor, never directly (turn it off under Notifications if you don't want it). It uses more " +
                            "battery than a push app, and a Tor reconnect can delay a notification by tens of seconds. A push says only " +
                            "that something arrived: never who from, which chat, or any text."
                    } else if (strictNoPush) {
                        "On: you only get messages and calls while the app is open. No notification arrives when it's closed, and a call to you rings only if the app is open."
                    } else {
                        "Off: notifications come through your UnifiedPush app (such as ntfy) if you have one, or else through Redvault's own " +
                            "background connection to this server. A push says only that something arrived (\"You have a new message\"): never " +
                            "who from, which chat, or any text, and it's encrypted for this phone. The push app talks to its own push server " +
                            (if (tor != null) "outside Tor, " else "") +
                            "so that server learns when pushes reach this phone; our server learns the push address and when it sends one." +
                            (if (tor != null) " The background connection goes through Tor like everything else." else "")
                    },
                    color = colors.text3, fontSize = 13.sp,
                )
            }
            Spacer(Modifier.height(16.dp))
        }
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
            Text(detail, color = colors.text3, fontSize = 12.5.sp, fontFamily = if (title == "Onion service") MonoFamily else null)
        }
        if (value.isNotEmpty()) Text(value, color = tone, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
    }
}

/**
 * The thin strip above the tabs (Tor routing only): Tor connecting / connected / unavailable, what's
 * waiting, and, during a call, that it goes WebRTC through the TURN relay rather than Tor.
 */
@Composable
fun TorStatusStrip(status: TorStatus, queued: Int, pausedUploads: Int, callActive: Boolean) {
    val colors = RedvaultTheme.colors
    val (text, tone) = when (status.state) {
        TorState.Connected -> "Tor connected" to colors.accent
        TorState.Connecting -> "Tor connecting · ${status.progress}%" to colors.warn
        TorState.Unavailable -> "Tor unavailable" to colors.danger
    }
    val extras = buildList {
        if (queued > 0) add("$queued queued")
        if (pausedUploads > 0) add("$pausedUploads upload${if (pausedUploads == 1) "" else "s"} paused")
        if (callActive) add("call via WebRTC + TURN relay")
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
