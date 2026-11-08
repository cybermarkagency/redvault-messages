// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
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
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.devicekeys.DeviceKeySafety
import com.redvaultsystems.redvaultmessage.devicekeys.DeviceKeysSession
import com.redvaultsystems.redvaultmessage.devicekeys.DeviceKeysState
import com.redvaultsystems.redvaultmessage.devicekeys.EngineException
import kotlinx.coroutines.launch
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.files.MonoFamily
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import com.redvaultsystems.redvaultmessage.ui.verify.qrBitmap
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import java.text.DateFormat
import java.time.Instant
import java.util.Date

/** What each security event means to the person reading it. */
private val EVENT_TEXT = mapOf(
    "device_registered" to "A device registered its keys (trusted only once approved)",
    "device_list_created" to "Device keys were set up for this account",
    "device_added" to "A device was approved",
    "device_removed" to "A device was removed",
    "device_unregistered" to "A device's keys were deleted",
    "enrollment_requested" to "A new device asked to pair",
    "account_key_rotated" to "The account key was replaced by a trusted device",
    "account_reset_requested" to "Someone asked to reset this account's keys",
    "account_reset_cancelled" to "A reset of this account's keys was cancelled",
    "account_reset_completed" to "This account's keys were reset",
    "recovery_backup_updated" to "A recovery backup was saved",
    "recovery_backup_downloaded" to "The recovery backup was downloaded",
    "signal_enabled" to "A device turned on Signal sessions (development build)",
)

@Composable
private fun Card(content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(RedvaultTheme.colors.surface), content = content)
}

@Composable
private fun Divider() = Box(Modifier.fillMaxWidth().height(1.dp).background(RedvaultTheme.colors.border))

@Composable
private fun Codes(codes: List<Int>) {
    Text(codes.joinToString("   ") { it.toString().padStart(4, '0') }, color = RedvaultTheme.colors.text, fontFamily = MonoFamily, fontSize = 22.sp, fontWeight = FontWeight.SemiBold)
}

@Composable
private fun Item(title: String, detail: String? = null, badge: String? = null, extra: @Composable () -> Unit = {}, actions: @Composable () -> Unit = {}) {
    val colors = RedvaultTheme.colors
    Row(Modifier.fillMaxWidth().padding(14.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(title, color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f, fill = false))
                if (badge != null) {
                    Text(
                        badge,
                        color = colors.accent,
                        fontSize = 11.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.clip(RoundedCornerShape(10.dp)).background(colors.accentSoft).padding(horizontal = 8.dp, vertical = 2.dp),
                    )
                }
            }
            if (detail != null) Text(detail, color = colors.text3, fontSize = 12.5.sp)
            extra()
        }
        actions()
    }
}

@Composable
private fun TextAction(text: String, danger: Boolean = false, enabled: Boolean = true, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Text(
        text,
        color = if (!enabled) colors.text3 else if (danger) colors.danger else colors.accent,
        fontSize = 14.sp,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.clickable(enabled = enabled, role = Role.Button, onClick = onClick).padding(8.dp),
    )
}

private fun protectionText(value: String) = when (value) {
    "hardware" -> "Protected by this phone's secure hardware (the Android Keystore)."
    "software" -> "Protected by the Android Keystore, in software: this phone has no secure hardware for keys."
    else -> "Kept in this app's private storage."
}

private fun eventDate(text: String): String = runCatching {
    DateFormat.getDateInstance(DateFormat.MEDIUM).format(Date.from(Instant.parse(text.take(10) + "T00:00:00Z")))
}.getOrDefault(text.take(10))

/**
 * Settings > Device keys (preview): per-device encryption keys (protocol 3), a development preview behind the
 * server's E2E_V3_ENABLED flag. Messages keep using the current encryption. Mirrors the web's panel.
 */
@Composable
fun DeviceKeysScreen(session: DeviceKeysSession, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    val state by session.state.collectAsStateWithLifecycle()
    var recoveryInput by remember { mutableStateOf("") }
    LaunchedEffect(Unit) { session.refresh() }
    DisposableEffect(Unit) { onDispose { session.clearRecoveryKey() } }

    DsPage("Device keys (preview)", onBack) {
        Text(
            "In development: each device gets its own encryption keys, and a new device is trusted only when one of your trusted devices approves it. Your messages still use the current encryption.",
            color = colors.text2,
            fontSize = 14.sp,
        )
        state.error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
        when (state.available) {
            null -> Text("Checking…", color = colors.text3, fontSize = 14.sp)
            false -> Callout("Device keys are off: this server hasn't turned the preview on, or this build doesn't include the engine.", CalloutTone.Neutral, R.drawable.ui_shield)
            true -> Content(session, state, recoveryInput) { recoveryInput = it }
        }
    }
}

@Composable
private fun ColumnScope.Content(session: DeviceKeysSession, state: DeviceKeysState, recoveryInput: String, onRecoveryInput: (String) -> Unit) {
    val colors = RedvaultTheme.colors

    // Pending reset: first, because it is the one thing that needs attention now.
    state.pendingReset?.let { reset ->
        Callout(
            "Someone asked to reset this account's keys. It takes effect on ${reset.effectiveAt.take(16).replace('T', ' ')} UTC unless a trusted device cancels it. If it wasn't you, cancel it and change your password.",
            CalloutTone.Warn,
            R.drawable.ui_alert_circle,
        )
        if (state.hasDevice && state.trusted) PillButton("Cancel reset", session::cancelReset, Modifier.fillMaxWidth(), style = PillStyle.Danger, enabled = !state.busy)
    }

    // No device keys yet on this account: this phone can be the first.
    if (!state.hasDevice && !state.listExists) {
        Card {
            Item(
                "Set up device keys on this phone",
                "This phone becomes your first trusted device. Your current key vouches for the new one.",
                actions = { TextAction("Set up", enabled = !state.busy, onClick = session::setUp) },
            )
        }
    }

    // The account has trusted devices and this phone isn't one of them (yet).
    if ((!state.hasDevice || !state.linked) && state.listExists) {
        if (state.hasDevice && !state.joining) {
            Card {
                Item(
                    "Pairing was interrupted",
                    "This phone has keys but no trusted device has approved them yet. Start again.",
                    actions = { TextAction("Start over", enabled = !state.busy, onClick = session::forgetThisPhone) },
                )
            }
        }
        Card {
            Item(
                "Pair this phone with a trusted device",
                "Your password alone can't make this phone trusted. Open Device keys on a trusted device and compare the codes.",
                extra = {
                    if (state.joiningCodes != null) {
                        Codes(state.joiningCodes)
                        Text("Do these match the numbers on your trusted device?", color = colors.text3, fontSize = 12.5.sp)
                    } else if (state.joining) {
                        Text("Waiting for a trusted device to answer…", color = colors.text3, fontSize = 12.5.sp)
                    }
                },
                actions = {
                    if (!state.joining && !state.hasDevice) TextAction("Start pairing", enabled = !state.busy, onClick = session::startPairing)
                    if (state.joining && state.joiningCodes == null) TextAction("Cancel", danger = true, onClick = session::rejectJoin)
                },
            )
            if (!state.joining && !state.hasDevice) {
                Divider()
                Item(
                    "Link with a QR code",
                    "If your other phone is next to you: this phone shows a code and the other one scans it. No numbers to compare.",
                    actions = { TextAction("Show code", enabled = !state.busy, onClick = session::startQrLink) },
                )
            }
            if (state.qrOffer != null) {
                Divider()
                val code = remember(state.qrOffer) { qrBitmap(state.qrOffer, 720) }
                Column(Modifier.fillMaxWidth().padding(14.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Image(
                        bitmap = code,
                        contentDescription = "Device-link QR code",
                        modifier = Modifier.size(240.dp).background(Color.White, RoundedCornerShape(12.dp)).padding(8.dp),
                    )
                    Text(
                        "On your trusted phone, open Device keys and tap Scan a QR code. Only show this code to your own phone, and only in person.",
                        color = colors.text3,
                        fontSize = 12.5.sp,
                    )
                    PillButton("Cancel", session::cancelQrLink, Modifier.fillMaxWidth(), style = PillStyle.Secondary)
                }
            }
            if (state.joiningCodes != null) {
                Row(Modifier.fillMaxWidth().padding(start = 14.dp, end = 14.dp, bottom = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    PillButton("They match", session::confirmJoin, Modifier.weight(1f), enabled = !state.busy, busy = state.busy)
                    PillButton("They don't", session::rejectJoin, Modifier.weight(1f), style = PillStyle.Danger)
                }
            }
            Divider()
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("No trusted device left? Use your recovery key", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                DsTextField(recoveryInput, onRecoveryInput, "XXXX-XXXX-…", mono = true, label = "Recovery key")
                PillButton("Restore", { session.restore(recoveryInput); onRecoveryInput("") }, Modifier.fillMaxWidth(), style = PillStyle.Secondary, enabled = !state.busy && recoveryInput.isNotBlank())
            }
        }
    }

    // This phone was removed from the account's list.
    if (state.hasDevice && state.linked && !state.listedHere) {
        Callout("This phone was removed from your trusted devices.", CalloutTone.Warn, R.drawable.ui_alert_circle)
        PillButton("Delete its keys", session::forgetThisPhone, Modifier.fillMaxWidth(), style = PillStyle.Danger, enabled = !state.busy)
    }

    if (state.hasDevice && state.linked && state.listedHere) {
        Card {
            state.devices.forEachIndexed { i, d ->
                if (i > 0) Divider()
                Item(
                    "Trusted device",
                    d.fingerprint,
                    badge = listOfNotNull(if (d.thisPhone) "This phone" else null, if (d.signal) "Signal" else null).joinToString(" · ").ifEmpty { null },
                    actions = {
                        // A signed change: contacts are told, and chat with this device goes back to the device-key protocol.
                        if (d.signal && state.trusted) TextAction("Turn off Signal", enabled = !state.busy) { session.turnOffSignal(d.deviceId) }
                        if (!d.thisPhone && state.trusted) TextAction("Remove", danger = true, enabled = !state.busy) { session.revoke(d.deviceId) }
                    },
                )
            }
            state.waiting.forEach { w ->
                Divider()
                val answering = state.answeringDeviceId == w.deviceId
                Item(
                    "Signed in, not trusted",
                    if (w.asking) "Asking to pair. Compare the codes on both screens before approving." else "If you don't recognize it, remove it and change your password.",
                    extra = {
                        if (answering && state.answeringCodes != null) {
                            Codes(state.answeringCodes)
                            Text("Do these match the numbers on the new device?", color = colors.text3, fontSize = 12.5.sp)
                        }
                    },
                    actions = {
                        if (answering) {
                            // Approve and reject are below, full width.
                        } else {
                            if (state.trusted && w.asking && state.answeringDeviceId == null) TextAction("Compare codes", enabled = !state.busy) { session.compareCodes(w.deviceId) }
                            TextAction("Remove", danger = true, enabled = !state.busy) { session.removeUnapproved(w.deviceId) }
                        }
                    },
                )
                if (answering && state.answeringCodes != null) {
                    Row(Modifier.fillMaxWidth().padding(start = 14.dp, end = 14.dp, bottom = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        PillButton("They match", session::confirmAnswer, Modifier.weight(1f), enabled = !state.busy, busy = state.busy)
                        PillButton("They don't", session::rejectAnswer, Modifier.weight(1f), style = PillStyle.Danger)
                    }
                }
            }
        }

        if (state.trusted) {
            Card {
                Item(
                    "Recovery key",
                    "For when you have no trusted device left. Write it down; it's shown once and never stored here or on the server.",
                    extra = { state.recoveryKey?.let { Text(it, color = colors.text, fontFamily = MonoFamily, fontSize = 14.sp) } },
                    actions = { TextAction(if (state.recoveryKey != null) "Make a new one" else "Create", enabled = !state.busy, onClick = session::createRecoveryKey) },
                )
            }
        }

        if (state.trusted) {
            val scan = rememberLauncherForActivityResult(ScanContract()) { result ->
                result.contents?.let { session.approveScanned(it) }
            }
            Card {
                Item(
                    "Link another phone by QR code",
                    "Open Device keys on the new phone, tap Show code, and scan it here. Only link a phone that is in your hands.",
                    actions = {
                        TextAction("Scan a QR code", enabled = !state.busy) {
                            scan.launch(
                                ScanOptions()
                                    .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                                    .setPrompt("Scan the code shown on your new phone")
                                    .setBeepEnabled(false)
                                    .setOrientationLocked(false),
                            )
                        }
                    },
                )
            }
            if (state.qrLinked) Callout("Linked. The new phone is now trusted.", CalloutTone.Ok, R.drawable.ui_shield_check)
        }

        Text(protectionText(state.protection), color = colors.text3, fontSize = 12.5.sp)
    }

    if (state.events.isNotEmpty()) {
        Card {
            state.events.forEachIndexed { i, e ->
                if (i > 0) Divider()
                Row(Modifier.fillMaxWidth().padding(14.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text(EVENT_TEXT[e.kind] ?: e.kind, color = colors.text2, fontSize = 13.sp, modifier = Modifier.weight(1f))
                    Text(eventDate(e.createdOn), color = colors.text3, fontSize = 12.5.sp)
                }
            }
        }
    }
}

/** What the device-key section knows, kept by the Verify screen so its top can warn about a key change too. */
class DeviceKeySafetyState {
    var safety by mutableStateOf<DeviceKeySafety?>(null)
    var problem by mutableStateOf<String?>(null)
    var scanNote by mutableStateOf<Pair<String, CalloutTone>?>(null)
    var busy by mutableStateOf(false)
}

/** At the top of the Verify screen: the contact's device keys changed (the details are further down). */
@Composable
fun DeviceKeyChangedNotice(state: DeviceKeySafetyState) {
    if (state.safety?.keyChanged != true) return
    Callout(
        "Their device keys changed. Scroll down to Device keys and compare the new number before you trust them.",
        CalloutTone.Warn,
        R.drawable.ui_alert_circle,
    )
}

/**
 * The device-key safety number for a chat's Verify screen: shown only when the server has the preview on,
 * this phone has device keys and so does the contact.
 */
@Composable
fun DeviceKeySafetyNumberSection(
    session: DeviceKeysSession,
    contactUserId: String,
    conversationId: String?,
    state: DeviceKeySafetyState = remember(contactUserId) { DeviceKeySafetyState() },
) {
    val scope = androidx.compose.runtime.rememberCoroutineScope()
    var safety by state::safety
    var problem by state::problem
    var scanNote by state::scanNote
    var busy by state::busy

    // Runs a change (or just a reload) and shows the new state; a failure becomes a note, never a crash.
    fun update(change: suspend () -> DeviceKeySafety?) {
        busy = true
        scope.launch {
            try {
                safety = change()
                problem = null
            } catch (e: EngineException) {
                problem = if (e.code == "account_key_changed") {
                    "Their keys changed again while this was open. Go back and open Verify again."
                } else {
                    "Couldn't check their device keys. Try again."
                }
            } catch (_: Exception) {
                problem = "Couldn't reach the server. Try again."
            } finally {
                busy = false
            }
        }
    }
    LaunchedEffect(contactUserId) { update { session.safety(contactUserId, conversationId) } }

    val scan = rememberLauncherForActivityResult(ScanContract()) { result ->
        val contents = result.contents ?: return@rememberLauncherForActivityResult
        val current = safety ?: return@rememberLauncherForActivityResult
        if (!contents.trim().startsWith("RVDK1:")) {
            scanNote = "That isn't a device-key code. Scan the one under Device keys on their Verify screen." to CalloutTone.Warn
            return@rememberLauncherForActivityResult
        }
        update {
            val (matched, after) = session.scanSafetyCode(contactUserId, conversationId, current.accountKey, contents)
            scanNote = if (matched) {
                "It matches. Their device keys are verified on this phone." to CalloutTone.Ok
            } else {
                "Those don't match. Someone may be in the middle, or one of you has new keys. Don't trust this chat until you know why." to CalloutTone.Error
            }
            after
        }
    }

    problem?.let { Callout(it, CalloutTone.Warn, R.drawable.ui_alert_circle) }
    val current = safety ?: return
    DeviceKeySafetyContent(
        current = current,
        busy = busy,
        scanNote = scanNote,
        onScan = {
            scanNote = null
            scan.launch(
                ScanOptions()
                    .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                    .setPrompt("Scan the code under Device keys on their Verify screen")
                    .setBeepEnabled(false)
                    .setOrientationLocked(false),
            )
        },
        onMarkVerified = { verified -> update { session.setSafetyVerified(contactUserId, conversationId, current.accountKey, verified) } },
        onAcceptNewKeys = { verified -> update { session.acceptNewKeys(contactUserId, conversationId, current.accountKey, verified) } },
    )
}

/** What the device-key part of the Verify screen draws for one state; the actions are the caller's. */
@Composable
fun DeviceKeySafetyContent(
    current: DeviceKeySafety,
    busy: Boolean,
    scanNote: Pair<String, CalloutTone>?,
    onScan: () -> Unit,
    onMarkVerified: (Boolean) -> Unit,
    onAcceptNewKeys: (Boolean) -> Unit,
) {
    val colors = RedvaultTheme.colors

    Text("Device keys", color = colors.text, fontWeight = FontWeight.SemiBold, fontSize = 16.sp)
    if (current.keyChanged) {
        Callout(
            "Their device keys changed: they started over on a new device, or someone is in the middle. " +
                "Until you accept the new keys, messages with them stay on hold. Compare this new number first.",
            CalloutTone.Warn,
            R.drawable.ui_alert_circle,
        )
    }
    Text(
        "This number covers the keys on every device you both use. Scan their code, let them scan yours, or compare the digits.",
        color = colors.text2,
        fontSize = 14.sp,
    )
    scanNote?.let { (text, tone) -> Callout(text, tone, if (tone == CalloutTone.Ok) R.drawable.ui_shield_check else R.drawable.ui_alert_circle) }
    val qr = remember(current.qrText) { qrBitmap(current.qrText) }
    // Dark on white regardless of theme, so any camera can read it.
    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        Image(
            bitmap = qr,
            contentDescription = "Your device-key safety number code for this chat",
            modifier = Modifier.size(220.dp).background(Color.White, RoundedCornerShape(12.dp)).padding(8.dp),
        )
    }
    Text(
        current.groups.chunked(4).joinToString("\n") { it.joinToString("  ") },
        color = colors.text,
        fontFamily = MonoFamily,
        fontSize = 17.sp,
        modifier = Modifier.fillMaxWidth(),
        textAlign = androidx.compose.ui.text.style.TextAlign.Center,
    )
    if (current.verified) {
        Callout("You've verified their device keys on this phone.", CalloutTone.Ok, R.drawable.ui_shield_check)
    }
    PillButton(
        "Scan their device-key code",
        onScan,
        Modifier.fillMaxWidth(),
        enabled = !busy,
    )
    when {
        current.keyChanged -> {
            PillButton(
                "The new numbers match, trust the new keys",
                { onAcceptNewKeys(true) },
                Modifier.fillMaxWidth(),
                style = PillStyle.Secondary,
                enabled = !busy,
            )
            PillButton(
                "Trust the new keys without comparing",
                { onAcceptNewKeys(false) },
                Modifier.fillMaxWidth(),
                style = PillStyle.Secondary,
                enabled = !busy,
            )
        }
        current.verified -> PillButton(
            "Mark device keys as not verified",
            { onMarkVerified(false) },
            Modifier.fillMaxWidth(),
            style = PillStyle.Secondary,
            enabled = !busy,
        )
        else -> PillButton(
            "The digits match, mark device keys as verified",
            { onMarkVerified(true) },
            Modifier.fillMaxWidth(),
            style = PillStyle.Secondary,
            enabled = !busy,
        )
    }
}
