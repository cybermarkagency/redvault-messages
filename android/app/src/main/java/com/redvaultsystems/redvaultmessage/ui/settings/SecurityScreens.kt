// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
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
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.api.LogEntryDto
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.components.SearchBox
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.ui.components.SectionCaps
import com.redvaultsystems.redvaultmessage.ui.components.TouchGuard
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.files.MonoFamily
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/** A page in the design's style: back button, a display title, a scrolling body. */
@Composable
internal fun DsPage(title: String, onBack: () -> Unit, actions: @Composable () -> Unit = {}, content: @Composable ColumnScope.() -> Unit) {
    val colors = RedvaultTheme.colors
    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(start = 8.dp, end = 12.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            RoundButton(R.drawable.ui_chevron_left, "Back", onClick = onBack, container = Color.Transparent, iconSize = 24.dp)
            Spacer(Modifier.weight(1f))
            actions()
        }
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Text(title, fontFamily = RedvaultFonts.display, fontSize = 30.sp, fontWeight = FontWeight.Bold, color = colors.text)
            content()
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
private fun toneColor(tone: Tone) = when (tone) {
    Tone.Ok -> RedvaultTheme.colors.accent
    Tone.Warn -> RedvaultTheme.colors.warn
    Tone.Muted -> RedvaultTheme.colors.text3
}

// --- Linked devices ---

@Composable
fun LinkedDevicesScreen(security: SecuritySession, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    val state by security.devices.collectAsStateWithLifecycle()
    var renaming by remember { mutableStateOf<String?>(null) }
    var signingOut by remember { mutableStateOf<LinkedDevice?>(null) }
    var signingOutOthers by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { security.loadDevices() }

    DsPage("Linked devices", onBack) {
        Text(
            "Everywhere you're signed in. The server keeps no IP address, device details or sign-in time for any of them: only when each sign-in expires. Device names are encrypted by your own devices.",
            color = colors.text2,
            fontSize = 14.sp,
        )
        state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
        if (!state.loaded) Text("Loading…", color = colors.text3, fontSize = 14.sp)
        Card {
            state.devices.forEachIndexed { i, d ->
                if (i > 0) Divider()
                Row(Modifier.fillMaxWidth().padding(14.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Box(Modifier.size(40.dp).clip(RoundedCornerShape(12.dp)).background(colors.surface2), contentAlignment = Alignment.Center) {
                        DsIcon(if (d.name.startsWith("Android") || d.name.contains("phone", true)) R.drawable.ui_user else R.drawable.ui_monitor, if (d.current) colors.accent else colors.text2, 20.dp)
                    }
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text(d.name.ifEmpty { "Unnamed device" }, color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                            if (d.current) {
                                Text(
                                    "This device",
                                    color = colors.accent,
                                    fontSize = 11.sp,
                                    fontWeight = FontWeight.SemiBold,
                                    modifier = Modifier.clip(RoundedCornerShape(10.dp)).background(Color(0xFF16271F)).padding(horizontal = 8.dp, vertical = 2.dp),
                                )
                            }
                        }
                        Text(
                            if (d.awaitingTwoFactor) "Password entered, waiting for the two-factor code" else "Sign-in expires ${d.expiresOn}",
                            color = if (d.awaitingTwoFactor) colors.warn else colors.text3,
                            fontSize = 12.5.sp,
                        )
                    }
                    if (d.current) {
                        Text("Rename", color = colors.accent, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.clickable(role = Role.Button) { renaming = d.name }.padding(8.dp))
                    } else {
                        Text("Sign out", color = colors.danger, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.clickable(role = Role.Button) { signingOut = d }.padding(8.dp))
                    }
                }
            }
        }
        if (state.devices.count { !it.current } > 0) {
            PillButton("Sign out all other devices", { signingOutOthers = true }, Modifier.fillMaxWidth(), style = PillStyle.Danger, icon = R.drawable.ui_log_out)
        }
    }

    renaming?.let { current ->
        var name by remember { mutableStateOf(current) }
        AlertDialog(
            onDismissRequest = { renaming = null },
            containerColor = colors.surface2,
            title = { Text("Name this device", color = colors.text) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Only your devices can read it: it's encrypted before it's saved.", color = colors.text2, fontSize = 14.sp)
                    DsTextField(name, { if (it.length <= 60) name = it }, "Device name")
                }
            },
            confirmButton = { TextButton(enabled = name.isNotBlank(), onClick = { security.rename(name); renaming = null }) { Text("Save", color = colors.accent) } },
            dismissButton = { TextButton(onClick = { renaming = null }) { Text("Cancel", color = colors.text2) } },
        )
    }
    signingOut?.let { device ->
        AlertDialog(
            onDismissRequest = { signingOut = null },
            containerColor = colors.surface2,
            title = { Text("Sign out “${device.name.ifEmpty { "this device" }}”?", color = colors.text) },
            text = {
                TouchGuard()
                Text("That device will need your password (and two-factor code) to sign in again.", color = colors.text2)
            },
            confirmButton = { TextButton(onClick = { security.signOut(device.id); signingOut = null }) { Text("Sign it out", color = colors.danger) } },
            dismissButton = { TextButton(onClick = { signingOut = null }) { Text("Cancel", color = colors.text2) } },
        )
    }
    if (signingOutOthers) {
        AlertDialog(
            onDismissRequest = { signingOutOthers = false },
            containerColor = colors.surface2,
            title = { Text("Sign out every other device?", color = colors.text) },
            text = {
                TouchGuard()
                Text("Only this phone stays signed in. The others will need your password (and two-factor code) again.", color = colors.text2)
            },
            confirmButton = { TextButton(onClick = { security.signOutOthers(); signingOutOthers = false }) { Text("Sign them out", color = colors.danger) } },
            dismissButton = { TextButton(onClick = { signingOutOthers = false }) { Text("Cancel", color = colors.text2) } },
        )
    }
}

// --- Relay servers ---

@Composable
fun RelayServersScreen(security: SecuritySession, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    val state by security.relays.collectAsStateWithLifecycle()
    LaunchedEffect(Unit) { security.loadRelays() }

    DsPage("Relay servers", onBack) {
        Text(
            "Messages go through the message relay, and calls always go through a call relay, so nobody you talk to ever learns your IP address. Relays only ever carry ciphertext: they can't read messages or listen to calls.",
            color = colors.text2,
            fontSize = 14.sp,
        )
        PillButton("Check connections", security::checkRelays, Modifier.fillMaxWidth(), style = PillStyle.Secondary, icon = R.drawable.ui_refresh)
        SectionCaps("Message relay", edge = 0.dp)
        Card { RelayLine(state.apiHost, "HTTPS", state.apiStatus, toneColor(state.apiTone)) }
        SectionCaps("Call relays (TURN)", edge = 0.dp)
        state.error?.let { Callout(it, CalloutTone.Neutral, R.drawable.ui_info) }
        if (state.relays.isNotEmpty()) {
            Card {
                state.relays.forEachIndexed { i, r ->
                    if (i > 0) Divider()
                    RelayLine(r.host, r.transport, r.status, toneColor(r.tone))
                }
            }
        }
        Text(
            "A check only asks each relay for an address: no media, and nothing that identifies you.",
            color = colors.text3,
            fontSize = 13.sp,
        )
    }
}

@Composable
private fun RelayLine(host: String, transport: String, status: String, tone: Color) {
    val colors = RedvaultTheme.colors
    Row(Modifier.fillMaxWidth().padding(14.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Box(Modifier.size(10.dp).clip(RoundedCornerShape(5.dp)).background(tone))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(host, color = colors.text, fontFamily = MonoFamily, fontSize = 14.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(transport, color = colors.text3, fontSize = 12.5.sp)
        }
        Text(status, color = tone, fontSize = 13.sp)
    }
}

// --- Transparency log ---

@Composable
fun TransparencyLogScreen(security: SecuritySession, onBack: () -> Unit, onOpenFeed: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val state by security.log.collectAsStateWithLifecycle()
    var selected by remember { mutableStateOf<LogEntryDto?>(null) }
    var search by remember { mutableStateOf(state.search) }
    DisposableEffect(Unit) {
        security.openLog()
        onDispose { security.closeLog() }
    }

    DsPage("Transparency log", onBack, actions = {
        Text("Live feed", color = colors.accent, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.clickable(role = Role.Button, onClick = onOpenFeed).padding(10.dp))
    }) {
        Text(
            "Every relayed message adds a commitment to a public, append-only log. This phone checks the log itself: the signature, that history was never rewritten, and that your own messages are in it. The log never contains message content, senders or recipients. Only this phone can tell which entries are yours.",
            color = colors.text2,
            fontSize = 14.sp,
        )
        when {
            state.disabled -> Callout("This server doesn't run a transparency log.", CalloutTone.Neutral, R.drawable.ui_info)
            !state.loaded -> Text("Checking the log…", color = colors.text3, fontSize = 14.sp)
        }
        state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
        state.checkpoint?.let { cp ->
            val ok = cp.keyStatus != "changed" && cp.consistent != false
            Column(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(if (ok) Color(0xFF16271F) else colors.dangerSoft)
                    .border(1.dp, if (ok) Color(0xFF244536) else colors.danger, RoundedCornerShape(16.dp)).padding(14.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    DsIcon(if (ok) R.drawable.ui_shield_check else R.drawable.ui_alert_circle, if (ok) colors.accent else colors.danger, 18.dp)
                    Text(
                        when {
                            cp.keyStatus == "changed" -> "The log's signing key changed"
                            cp.consistent == false -> "The log's history doesn't match what this phone saw before"
                            else -> "Checkpoint verified on this phone"
                        },
                        color = if (ok) colors.accent else colors.danger,
                        fontSize = 15.sp,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                CheckLine("Signature", "Valid (Ed25519)")
                CheckLine("Signing key", cp.keyFingerprint + when (cp.keyStatus) { "first-use" -> " · trusted on first use"; "changed" -> " · CHANGED"; else -> " · same as before" })
                CheckLine("Append-only", when (cp.consistent) { true -> "Consistent with the last checkpoint seen here"; false -> "NOT consistent: rewritten or forked"; null -> "First check on this phone" })
                CheckLine("Entries", "${cp.treeSize}")
                CheckLine("Root", cp.rootHash.take(16) + "…" + cp.rootHash.takeLast(10), mono = true)
                CheckLine("Signed", cp.signedAt.replace('T', ' ').take(16) + " UTC")
                if (cp.keyStatus == "changed") {
                    Text("Only trust the new key if the operator has announced it. Its fingerprint is ${cp.keyFingerprint}.", color = colors.text2, fontSize = 13.sp)
                    PillButton("Trust new key ${cp.keyFingerprint}", security::acceptNewKey, Modifier.fillMaxWidth(), style = PillStyle.Danger, height = 42.dp)
                }
            }
            Card {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("Your messages", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                    Text(
                        when {
                            state.checkingMine -> "Checking your recent messages…"
                            state.mineMismatches > 0 -> "${state.mineMismatches} of your messages don't match the log. Treat it as untrusted."
                            state.mineMatched + state.minePending == 0 -> "None of your recent messages are logged yet."
                            else -> "${state.mineMatched} of your recent messages proven in the log" + if (state.minePending > 0) ", ${state.minePending} waiting for the next checkpoint." else "."
                        },
                        color = if (state.mineMismatches > 0) colors.danger else colors.text2,
                        fontSize = 13.5.sp,
                    )
                }
            }
        }
        if (!state.disabled && state.loaded) {
            SearchBox(search, { search = it; security.setSearch(it) }, "Search by #entry or commitment", R.drawable.ui_search)
            ChipRow(listOf(false, true), state.onlyMine, { if (it) "Only mine" else "All entries" }, security::setOnlyMine, edge = 0.dp)
            Card {
                if (state.rows.isEmpty()) Text(if (state.onlyMine) "None of your messages are in the log yet." else "No entries.", color = colors.text3, fontSize = 14.sp, modifier = Modifier.padding(14.dp))
                state.rows.forEachIndexed { i, row ->
                    if (i > 0) Divider()
                    Row(
                        Modifier.fillMaxWidth().clickable { selected = row.entry }.padding(horizontal = 14.dp, vertical = 10.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        Text("#${row.entry.index}", color = colors.text2, fontFamily = MonoFamily, fontSize = 13.sp, modifier = Modifier.size(width = 64.dp, height = 20.dp))
                        Column(Modifier.weight(1f)) {
                            Text(row.entry.commitment.take(12) + "…" + row.entry.commitment.takeLast(6), color = colors.text, fontFamily = MonoFamily, fontSize = 13.sp, maxLines = 1)
                            if (row.mine) Text("yours", color = colors.accent, fontSize = 12.sp)
                        }
                        StatusDot(row.status)
                    }
                }
            }
        }
    }

    selected?.let { entry ->
        var checking by remember(entry.index) { mutableStateOf(true) }
        var ok by remember(entry.index) { mutableStateOf(false) }
        var path by remember(entry.index) { mutableStateOf<List<String>>(emptyList()) }
        LaunchedEffect(entry.index) {
            val (good, p) = security.recheck(entry)
            ok = good
            path = p
            checking = false
        }
        AlertDialog(
            onDismissRequest = { selected = null },
            containerColor = colors.surface2,
            title = { Text("Entry #${entry.index}", color = colors.text) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        when {
                            checking -> "Checking its inclusion proof on this phone…"
                            ok -> "Included in the signed checkpoint: proof checked on this phone."
                            else -> "The inclusion proof didn't check out."
                        },
                        color = when { checking -> colors.text2; ok -> colors.accent; else -> colors.danger },
                        fontSize = 14.sp,
                    )
                    CheckLine("Commitment", entry.commitment, mono = true)
                    CheckLine("Scheme", "SHA-256 commitment v1 · RFC 9162 tree · Ed25519")
                    if (path.isNotEmpty()) Text("Proof path: ${path.size} hashes", color = colors.text3, fontSize = 13.sp)
                }
            },
            confirmButton = {
                TextButton(enabled = path.isNotEmpty(), onClick = {
                    SecretClipboard.copy(context, "{\"index\":${entry.index},\"treeSize\":${state.checkpoint?.treeSize ?: 0},\"path\":[${path.joinToString(",") { "\"$it\"" }}]}", secret = false)
                }) { Text("Copy proof", color = colors.accent) }
            },
            dismissButton = { TextButton(onClick = { selected = null }) { Text("Close", color = colors.text2) } },
        )
    }
}

@Composable
private fun CheckLine(label: String, value: String, mono: Boolean = false) {
    val colors = RedvaultTheme.colors
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(label, color = colors.text3, fontSize = 13.sp, modifier = Modifier.size(width = 96.dp, height = 18.dp))
        Text(value, color = colors.text, fontSize = 13.sp, fontFamily = if (mono) MonoFamily else RedvaultFonts.sans, modifier = Modifier.weight(1f))
    }
}

@Composable
private fun StatusDot(status: String) {
    val colors = RedvaultTheme.colors
    val (text, color) = when (status) {
        "verified" -> "Verified" to colors.accent
        "failed" -> "Failed" to colors.danger
        "mismatch" -> "Mismatch" to colors.danger
        else -> "Checking" to colors.text3
    }
    Text(text, color = color, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
}
