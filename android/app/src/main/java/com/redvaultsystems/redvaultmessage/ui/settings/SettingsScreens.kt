// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.ui.text.font.FontWeight
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.data.LockTimeout
import com.redvaultsystems.redvaultmessage.data.api.FileStorageUsageDto
import com.redvaultsystems.redvaultmessage.ui.lock.AppLockRow
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.redvaultsystems.redvaultmessage.data.api.MfaSetupResult
import com.redvaultsystems.redvaultmessage.push.PushMode
import com.redvaultsystems.redvaultmessage.data.api.PlanStatusDto
import java.time.LocalDate
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle
import com.redvaultsystems.redvaultmessage.ui.account.AccountSession
import com.redvaultsystems.redvaultmessage.ui.account.Profile
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.RvSwitch
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.ui.files.MonoFamily
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.NoticeTone
import com.redvaultsystems.redvaultmessage.ui.components.PasswordInput
import com.redvaultsystems.redvaultmessage.ui.components.PrimaryButton
import com.redvaultsystems.redvaultmessage.ui.components.SectionLabel
import com.redvaultsystems.redvaultmessage.ui.components.SettingsRow
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.launch

private const val MIN_USERNAME_LENGTH = 3
private const val MAX_USERNAME_LENGTH = 64
private const val MIN_PASSWORD_LENGTH = 8

/** Port of ui/src/screens/Settings.tsx, minus the web-only prototype toggles. */
@Composable
fun SettingsScreen(
    account: AccountSession,
    profile: Profile,
    incomingRequests: Int,
    readReceipts: Boolean,
    onReadReceiptsChange: (Boolean) -> Unit,
    hideContent: Boolean,
    onHideContentChange: (Boolean) -> Unit,
    notificationsOn: Boolean,
    onNotificationsChange: (Boolean) -> Unit,
    /** How notifications reach this phone right now (push/PushRegistrar.kt). */
    pushMode: PushMode = PushMode.Off,
    /** A distributor package's app name, for "Push through ntfy". */
    distributorName: (String) -> String = { it },
    /** How many UnifiedPush apps are installed: none gets a note, several a "Choose push app" row. */
    distributorCount: Int = 0,
    backgroundConnection: Boolean = true,
    onBackgroundConnectionChange: (Boolean) -> Unit = {},
    onPickDistributor: () -> Unit = {},
    onBack: () -> Unit,
    onOpenFriendRequests: () -> Unit,
    onOpenChangeUsername: () -> Unit,
    onOpenChangePassword: () -> Unit,
    onOpenMfa: () -> Unit,
    onOpenTransparency: () -> Unit,
    appLockEnabled: Boolean,
    appLockTimeout: LockTimeout,
    onOpenAppLock: () -> Unit,
    onOpenPrivacyMode: () -> Unit,
    onOpenNetworkRouting: () -> Unit = {},
    /** This build talks to an onion service: the routing row says Tor. */
    torRouting: Boolean = false,
    blockedCount: Int,
    onOpenBlocked: () -> Unit,
    /** Unread messages and polls from the team, and the screen that shows them. */
    inboxUnread: Int = 0,
    onOpenInbox: () -> Unit = {},
    onOpenRecoveryKey: () -> Unit,
    onOpenDeleteAccount: () -> Unit,
    onLogout: () -> Unit,
    typingIndicators: Boolean = false,
    onTypingIndicatorsChange: (Boolean) -> Unit = {},
    onOpenLinkedDevices: () -> Unit = {},
    /** Per-device keys (development builds only); null hides the row. */
    onOpenDeviceKeys: (() -> Unit)? = null,
    onOpenRelayServers: () -> Unit = {},
    onOpenTransparencyLog: () -> Unit = {},
    ringtone: Boolean = true,
    onRingtoneChange: (Boolean) -> Unit = {},
    echoCancellation: Boolean = true,
    onEchoCancellationChange: (Boolean) -> Unit = {},
    /** Story settings; null while the server has Stories off (then there is nothing to show). */
    onOpenStorySettings: (() -> Unit)? = null,
) {
    val scope = rememberCoroutineScope()
    var avatarBusy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    fun run(fallback: String, action: suspend () -> Unit) {
        error = null
        scope.launch {
            try {
                action()
            } catch (e: Exception) {
                error = describeError(e, fallback)
            }
        }
    }

    // The system photo picker: no storage permission needed, and the app
    // only ever sees the one image the user picks.
    val appLock = LocalAppLock.current
    val pickPhoto = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        avatarBusy = true
        run("Couldn't use that image.") {
            try {
                account.uploadAvatar(uri)
            } finally {
                avatarBusy = false
            }
        }
    }

    val colors = RedvaultTheme.colors
    SubScreen(title = "Settings", onBack = onBack) {
        // You: photo, handle and plan.
        Row(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Avatar(avatarUrl = profile.avatarUrl, seed = profile.handle, size = 64.dp)
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(profile.handle, fontFamily = MonoFamily, fontSize = 17.sp, color = colors.text, maxLines = 1)
                Text(
                    when (profile.subscriptionTier) { "premium" -> "Premium"; "pro" -> "Pro"; else -> "Free" },
                    color = colors.accent,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                    Text(
                        if (avatarBusy) "Uploading…" else if (profile.avatarUrl != null) "Change photo" else "Add photo",
                        color = colors.accent,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.clickable(enabled = !avatarBusy, role = Role.Button) {
                            appLock?.expectExternalReturn()
                            pickPhoto.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                        }.padding(vertical = 4.dp),
                    )
                    if (profile.avatarUrl != null) {
                        Text(
                            "Remove",
                            color = colors.text3,
                            fontSize = 14.sp,
                            modifier = Modifier.clickable(enabled = !avatarBusy, role = Role.Button) {
                                avatarBusy = true
                                run("Couldn't remove that photo. Try again.") {
                                    try {
                                        account.removeAvatar()
                                    } finally {
                                        avatarBusy = false
                                    }
                                }
                            }.padding(vertical = 4.dp),
                        )
                    }
                }
            }
        }
        Text("Your photo is visible to every user. It isn't encrypted or private.", color = colors.text3, fontSize = 12.5.sp)
        error?.let { Notice(it) }

        SectionLabel("Privacy")
        SettingsRow(
            title = "Maximum privacy",
            description = "See and turn on every privacy protection at once",
            onClick = onOpenPrivacyMode,
            icon = R.drawable.ui_shield_check,
        )
        SettingsRow(
            title = "Network routing",
            description = if (torRouting) "Messages, photos, files, Stories and voice messages: Tor only. Calls: WebRTC + TURN relay"
            else "How messages, files and calls leave this phone",
            onClick = onOpenNetworkRouting,
            icon = R.drawable.ui_globe,
        )
        AppLockRow(appLockEnabled, appLockTimeout, onOpenAppLock)
        SettingsRow(
            title = "Read receipts",
            description = "Let contacts see when you've read a message",
            icon = R.drawable.ui_check,
            trailing = { RvSwitch(checked = readReceipts, onCheckedChange = onReadReceiptsChange, description = "Read receipts") },
        )
        SettingsRow(
            title = "Show when I'm online",
            description = "Contacts who share theirs see when you're using the app, and you see theirs. Off: neither",
            icon = R.drawable.ui_signal,
            trailing = {
                RvSwitch(
                    checked = profile.showOnlineStatus == true,
                    enabled = profile.showOnlineStatus != null,
                    onCheckedChange = { on -> run("Couldn't change that. Try again.") { account.setShowOnlineStatus(on) } },
                    description = "Show when I'm online",
                )
            },
        )
        SettingsRow(
            title = "Typing indicators",
            description = "Show when you're typing, and see when others are. Off: neither",
            icon = R.drawable.ui_dots,
            trailing = { RvSwitch(checked = typingIndicators, onCheckedChange = onTypingIndicatorsChange, description = "Typing indicators") },
        )
        SettingsRow(
            title = "Hide app content",
            description = "Block screenshots and hide chats in the Recents screen",
            icon = R.drawable.ui_eye_off,
            trailing = { RvSwitch(checked = hideContent, onCheckedChange = onHideContentChange, description = "Hide app content") },
        )

        SectionLabel("Notifications and calls")
        SettingsRow(
            title = "Notifications",
            description = "Tell me when something new arrives. Never shows who it's from or what it says",
            icon = R.drawable.ui_bell,
            trailing = { RvSwitch(checked = notificationsOn, onCheckedChange = onNotificationsChange, description = "Notifications") },
        )
        if (notificationsOn) {
            SettingsRow(
                title = "How they arrive",
                description = when (pushMode) {
                    is PushMode.UnifiedPush -> if (pushMode.distributor.isEmpty()) "Push through your push app" else "Push through ${distributorName(pushMode.distributor)}"
                    PushMode.BackgroundConnection -> "Background connection"
                    PushMode.Off -> "Off"
                },
                icon = R.drawable.ui_info,
            )
            SettingsRow(
                title = "Background connection when no push app is installed",
                description = "Keeps a connection to the server open, with a quiet ongoing notification. Uses more battery than a push app",
                icon = R.drawable.ui_bell,
                trailing = {
                    RvSwitch(checked = backgroundConnection, onCheckedChange = onBackgroundConnectionChange, description = "Background connection when no push app is installed")
                },
            )
            if (distributorCount == 0) {
                Text(
                    "Installing a UnifiedPush app such as ntfy saves battery: RedVault then doesn't need its own connection, and still never uses Google's push service.",
                    color = colors.text3, fontSize = 12.5.sp,
                )
            } else if (distributorCount > 1) {
                SettingsRow(
                    title = "Choose push app",
                    description = "$distributorCount UnifiedPush apps are installed",
                    onClick = onPickDistributor,
                    icon = R.drawable.ui_bell,
                )
            }
        }
        SettingsRow(
            title = "Ringtone",
            description = "Ring for incoming calls. Muted contacts never ring",
            icon = R.drawable.ui_phone,
            trailing = { RvSwitch(checked = ringtone, onCheckedChange = onRingtoneChange, description = "Ringtone") },
        )
        SettingsRow(
            title = "Echo cancellation",
            description = "Stops the other person hearing themselves through your speaker. Leave on unless you use a headset and the line sounds muffled",
            icon = R.drawable.ui_headphones,
            trailing = { RvSwitch(checked = echoCancellation, onCheckedChange = onEchoCancellationChange, description = "Echo cancellation") },
        )

        SectionLabel("Friends")
        SettingsRow(
            title = "Accept messages from anyone",
            description = "Off rejects new friend requests from people you haven't talked to before",
            icon = R.drawable.ui_user_plus,
            trailing = {
                RvSwitch(
                    checked = profile.acceptMessageRequests ?: true,
                    enabled = profile.acceptMessageRequests != null,
                    onCheckedChange = { run("Couldn't update that setting. Try again.") { account.setAcceptMessageRequests(it) } },
                    description = "Accept messages from anyone",
                )
            },
        )
        SettingsRow(
            title = "Friend requests",
            description = if (incomingRequests > 0) "$incomingRequests waiting for you" else "Review, accept or deny requests",
            onClick = onOpenFriendRequests,
            icon = R.drawable.ui_users,
        )
        SettingsRow(
            title = "Blocked users",
            description = if (blockedCount == 0) "Nobody blocked" else "$blockedCount blocked",
            onClick = onOpenBlocked,
            icon = R.drawable.ui_ban,
        )
        SettingsRow(
            title = "Inbox",
            description = if (inboxUnread > 0) "$inboxUnread unread from the Redvault team" else "Messages and polls from the Redvault team",
            onClick = onOpenInbox,
            icon = R.drawable.ui_bell,
        )

        if (onOpenStorySettings != null) {
            SettingsRow(title = "Stories", description = "Who sees your stories, archive and highlights", onClick = onOpenStorySettings, icon = R.drawable.ui_image)
        }

        SectionLabel("Account")
        SettingsRow(title = "Handle", description = profile.handle, onClick = onOpenChangeUsername, icon = R.drawable.ui_user)
        PlanRow(account, profile.subscriptionTier)
        if (isPaidTier(profile.subscriptionTier)) StorageRow(account)

        SectionLabel("Security")
        SettingsRow(
            title = "Recovery key",
            description = when (profile.hasRecoveryKey) {
                false -> "Not set up: a forgotten password can't be recovered"
                else -> "The only way back in if you forget your password"
            },
            onClick = onOpenRecoveryKey,
            icon = R.drawable.ui_key,
        )
        SettingsRow(title = "Change password", description = "Re-encrypts your private key with the new password", onClick = onOpenChangePassword, icon = R.drawable.ui_lock)
        SettingsRow(
            title = "Two-factor authentication",
            description = if (profile.mfaEnabled) "On. Tap to turn off" else "Off. Set up an authenticator app",
            onClick = onOpenMfa,
            icon = R.drawable.ui_shield,
        )
        SettingsRow(title = "Linked devices", description = "Everywhere you're signed in; sign any of them out", onClick = onOpenLinkedDevices, icon = R.drawable.ui_monitor)
        if (onOpenDeviceKeys != null) {
            SettingsRow(title = "Device keys (preview)", description = "Per-device encryption keys; in development", onClick = onOpenDeviceKeys, icon = R.drawable.ui_shield)
        }

        SectionLabel("Network")
        SettingsRow(title = "Relay servers", description = "The message and call relays, and a connection check", onClick = onOpenRelayServers, icon = R.drawable.ui_globe)
        SettingsRow(
            title = "Transparency log",
            description = "Check on this phone that the log is signed, append-only, and holds your own messages",
            onClick = onOpenTransparencyLog,
            icon = R.drawable.ui_log,
        )
        SettingsRow(title = "Live transparency feed", description = "The public record of relayed messages", onClick = onOpenTransparency, icon = R.drawable.ui_signal)
        SettingsRow(
            title = "Transparency mode",
            description = "When on, your outgoing messages' ciphertext (never the readable content) appears in the public transparency feed",
            icon = R.drawable.ui_eye,
            trailing = {
                RvSwitch(
                    checked = profile.transparencyMode ?: false,
                    enabled = profile.transparencyMode != null,
                    onCheckedChange = { run("Couldn't update that setting. Try again.") { account.setTransparencyMode(it) } },
                    description = "Transparency mode",
                )
            },
        )

        OpenSourceSection()

        Spacer(Modifier.height(8.dp))
        PillButton("Log out", onLogout, Modifier.fillMaxWidth(), style = PillStyle.Secondary, icon = R.drawable.ui_log_out)
        PillButton("Delete account", onOpenDeleteAccount, Modifier.fillMaxWidth(), style = PillStyle.Quiet)
    }
}

/** Permanent self-service deletion, as Google Play requires for apps with accounts. */
@Composable
fun DeleteAccountScreen(account: AccountSession, onBack: () -> Unit, onDeleted: () -> Unit) {
    val scope = rememberCoroutineScope()
    var password by remember { mutableStateOf("") }
    var confirmed by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    SubScreen(title = "Delete account", onBack = onBack) {
        Text(
            "This permanently deletes your account and signs you out on every device. Your private key, recovery key, " +
                "two-factor setup, profile photo and notification tokens are removed. You can't undo this, and your " +
                "username can't be used again.",
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        Text(
            "Messages you already sent stay in the other people's chats, still end-to-end encrypted. They'll see your account as deleted.",
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        PasswordInput(password, { password = it }, "Password")
        Row(verticalAlignment = Alignment.CenterVertically) {
            Checkbox(checked = confirmed, onCheckedChange = { confirmed = it })
            Text("I understand this can't be undone", color = RedvaultTheme.colors.text, style = MaterialTheme.typography.bodyMedium)
        }
        error?.let { Notice(it) }
        PrimaryButton(
            text = if (busy) "Deleting…" else "Delete my account",
            enabled = password.isNotEmpty() && confirmed,
            busy = busy,
        ) {
            busy = true
            error = null
            scope.launch {
                try {
                    account.deleteAccount(password)
                    onDeleted()
                } catch (e: Exception) {
                    error = describeError(e, "Couldn't delete the account. Try again.")
                    busy = false
                }
            }
        }
    }
}

/** Pro or Premium: Premium includes everything Pro has (calls are web-only for now). */
fun isPaidTier(tier: String): Boolean = tier == "pro" || tier == "premium"

/** "2026-10-26" in the phone's own date format. */
private fun formatDay(day: String): String =
    runCatching { LocalDate.parse(day).format(DateTimeFormatter.ofLocalizedDate(FormatStyle.LONG)) }.getOrDefault(day)

/**
 * The plan, where it's headed, and for a card subscriber "Switch to Free"
 * (it stops renewing; Pro lasts until the paid period ends) or, once
 * that's chosen, "Keep Pro" to undo it.
 */
@Composable
private fun PlanRow(account: AccountSession, tier: String) {
    val scope = rememberCoroutineScope()
    val colors = RedvaultTheme.colors
    var status by remember { mutableStateOf<PlanStatusDto?>(null) }
    var confirming by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    // An older server has no /billing/status; the row then shows the tier alone.
    LaunchedEffect(tier) { status = runCatching { account.planStatus() }.getOrNull() }

    fun change(action: suspend () -> PlanStatusDto) {
        busy = true
        error = null
        scope.launch {
            try {
                status = action()
                confirming = false
            } catch (e: Exception) {
                error = describeError(e, "Couldn't change your plan. Try again.")
            } finally {
                busy = false
            }
        }
    }

    val current = status
    val plan = if (tier == "premium") "Premium" else "Pro"
    val description = when {
        !isPaidTier(tier) -> "Free: unlimited messages, open files others send. Sending files needs Pro"
        current?.endsOn != null ->
            "$plan until ${formatDay(current.endsOn)}, then Free" + if (current.source == "crypto") " (prepaid, nothing renews)" else ""
        current?.renews == true -> "$plan · renews monthly"
        else -> plan
    }
    SettingsRow(title = "Plan", description = description, icon = R.drawable.ui_star)
    if (!isPaidTier(tier) || current == null) return

    if (confirming) {
        AlertDialog(
            onDismissRequest = { if (!busy) confirming = false },
            title = { Text("Switch to Free?") },
            text = {
                Text(
                    "Your subscription stops renewing and you won't be charged again. You keep Pro until the end of the " +
                        "period you've paid for, then move to Free: messages only, no sending files. Files you've already " +
                        "sent stay available until they expire.",
                )
            },
            confirmButton = {
                TextButton(enabled = !busy, onClick = { change(account::cancelPlan) }) {
                    Text(if (busy) "Switching…" else "Switch to Free", color = colors.danger)
                }
            },
            dismissButton = { TextButton(enabled = !busy, onClick = { confirming = false }) { Text("Keep Pro") } },
        )
    }
    if (current.canResume) {
        Text("Your subscription won't renew. You won't be charged again.", color = colors.text2, style = MaterialTheme.typography.bodySmall)
        OutlinedButton(onClick = { change(account::resumePlan) }, enabled = !busy, shape = MaterialTheme.shapes.small, modifier = Modifier.fillMaxWidth()) {
            Text("Keep Pro")
        }
    } else if (current.canCancel) {
        TextButton(onClick = { error = null; confirming = true }) { Text("Switch to Free…", color = colors.danger) }
    }
    error?.let { Notice(it) }
}

/** Pro only: your file storage in use. Hidden if file sharing isn't set up on the server. */
@Composable
private fun StorageRow(account: AccountSession) {
    var usage by remember { mutableStateOf<FileStorageUsageDto?>(null) }
    LaunchedEffect(Unit) { usage = runCatching { account.storageUsage() }.getOrNull() }
    val current = usage ?: return
    val colors = RedvaultTheme.colors
    val fraction = if (current.limit > 0) (current.used.toFloat() / current.limit).coerceIn(0f, 1f) else 0f
    Column(
        Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(12.dp)).padding(horizontal = 16.dp, vertical = 14.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text("File storage", color = colors.text, fontWeight = FontWeight.Medium)
        Text(
            "${FileMessage.formatBytes(current.used)} of ${FileMessage.formatBytes(current.limit)} used · " +
                "${FileMessage.formatBytes(maxOf(0, current.limit - current.used))} left",
            color = colors.text2,
            style = MaterialTheme.typography.bodySmall,
        )
        LinearProgressIndicator(
            progress = { fraction },
            color = if (current.used >= current.limit) colors.danger else colors.accent,
            trackColor = colors.surface2,
            modifier = Modifier.fillMaxWidth(),
        )
    }
}

@Composable
fun ChangeUsernameScreen(account: AccountSession, current: String, onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    var username by rememberSaveable { mutableStateOf(current) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var done by remember { mutableStateOf(false) }
    val trimmed = username.trim()
    val valid = trimmed.length in MIN_USERNAME_LENGTH..MAX_USERNAME_LENGTH && trimmed != current

    SubScreen(title = "Change handle", onBack = onBack) {
        Text(
            "Your friends see the new handle. Anyone adding you from now on has to use it.",
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        OutlinedTextField(
            value = username,
            onValueChange = { if (it.length <= MAX_USERNAME_LENGTH) username = it },
            label = { Text("Handle") },
            singleLine = true,
            textStyle = MaterialTheme.typography.bodyLarge.copy(fontFamily = FontFamily.Monospace),
            keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
            modifier = Modifier.fillMaxWidth(),
        )
        error?.let { Notice(it) }
        if (done) Notice("Your handle is now $current.", NoticeTone.Success)
        PrimaryButton(text = "Save", enabled = valid, busy = busy) {
            busy = true
            error = null
            done = false
            scope.launch {
                try {
                    account.changeUsername(trimmed)
                    done = true
                } catch (e: Exception) {
                    error = describeError(e, "Couldn't change your handle.")
                } finally {
                    busy = false
                }
            }
        }
    }
}

/** Port of ui/src/screens/ChangePassword.tsx. */
@Composable
fun ChangePasswordScreen(account: AccountSession, onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    // Plain remember: passwords never go into saved instance state.
    var current by remember { mutableStateOf("") }
    var next by remember { mutableStateOf("") }
    var confirm by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var done by remember { mutableStateOf(false) }
    val valid = current.isNotEmpty() && next.length >= MIN_PASSWORD_LENGTH && next == confirm

    SubScreen(title = "Change password", onBack = onBack) {
        Text(
            "Your private key is re-encrypted with the new password on this phone before anything is sent. " +
                "Other devices are signed out. There's still no way to recover a forgotten password.",
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        PasswordInput(current, { current = it }, "Current password")
        PasswordInput(next, { next = it }, "New password (min. $MIN_PASSWORD_LENGTH characters)")
        PasswordInput(confirm, { confirm = it }, "Confirm new password")
        if (confirm.isNotEmpty() && confirm != next) Notice("New passwords don't match.")
        error?.let { Notice(it) }
        if (done) Notice("Password changed.", NoticeTone.Success)
        PrimaryButton(text = if (busy) "Changing…" else "Change password", enabled = valid, busy = busy) {
            busy = true
            error = null
            done = false
            scope.launch {
                try {
                    account.changePassword(current, next)
                    done = true
                    current = ""
                    next = ""
                    confirm = ""
                } catch (e: Exception) {
                    error = describeError(e, "Couldn't change your password.")
                } finally {
                    busy = false
                }
            }
        }
    }
}

/** Ports of ui/src/screens/MfaSetup.tsx and MfaDisable.tsx. */
@Composable
fun MfaScreen(account: AccountSession, enabled: Boolean, onBack: () -> Unit) {
    // Decided once on entry, so finishing setup shows its success message
    // instead of flipping straight to the "turn off" screen.
    val startedEnabled = remember { enabled }
    if (startedEnabled) MfaDisable(account, onBack) else MfaSetup(account, onBack)
}

@Composable
private fun MfaSetup(account: AccountSession, onBack: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var setup by remember { mutableStateOf<MfaSetupResult?>(null) }
    var code by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var done by remember { mutableStateOf(false) }
    var copied by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        try {
            setup = account.startMfaSetup()
        } catch (e: Exception) {
            error = describeError(e, "Couldn't start two-factor setup. Try again.")
        }
    }

    SubScreen(title = "Two-factor authentication", onBack = onBack) {
        if (done) {
            Notice("Two-factor authentication is on. You'll need a code from your authenticator app each time you log in.", NoticeTone.Success)
            return@SubScreen
        }
        Text(
            "Add this account to an authenticator app (like Google Authenticator or Authy), then enter the 6-digit code it shows.",
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        error?.let { Notice(it) }
        val current = setup
        if (current == null && error == null) Text("Loading…", color = RedvaultTheme.colors.text3)
        if (current != null) {
            // On the phone itself there's no QR to scan: hand the otpauth://
            // link straight to an installed authenticator app instead.
            PrimaryButton(text = "Open in authenticator app", enabled = true) {
                try {
                    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(current.otpauthUri)))
                } catch (_: ActivityNotFoundException) {
                    error = "No authenticator app found. Install one, or enter the key below by hand."
                }
            }
            Text("Or enter this key by hand:", color = RedvaultTheme.colors.text2, style = MaterialTheme.typography.bodyMedium)
            Text(
                current.secretBase32.chunked(4).joinToString(" "),
                fontFamily = FontFamily.Monospace,
                style = MaterialTheme.typography.titleMedium,
                textAlign = TextAlign.Center,
                color = RedvaultTheme.colors.text,
                modifier = Modifier.fillMaxWidth(),
            )
            TextButton(onClick = {
                SecretClipboard.copy(context, current.secretBase32, secret = true)
                copied = true
            }) { Text(if (copied) "Copied. Clears in 30 seconds" else "Copy key") }

            OutlinedTextField(
                value = code,
                onValueChange = { code = it.filter(Char::isDigit).take(6) },
                label = { Text("6-digit code") },
                singleLine = true,
                textStyle = MaterialTheme.typography.titleMedium.copy(fontFamily = FontFamily.Monospace),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
                modifier = Modifier.fillMaxWidth(),
            )
            PrimaryButton(text = if (busy) "Confirming…" else "Confirm and turn on", enabled = code.length == 6, busy = busy) {
                busy = true
                error = null
                scope.launch {
                    try {
                        account.confirmMfa(code)
                        done = true
                    } catch (e: Exception) {
                        error = "Wrong code. Check your authenticator app and try again."
                    } finally {
                        busy = false
                    }
                }
            }
        }
    }
}

@Composable
private fun MfaDisable(account: AccountSession, onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    var password by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var done by remember { mutableStateOf(false) }

    SubScreen(title = "Two-factor authentication", onBack = onBack) {
        if (done) {
            Notice("Two-factor authentication is off.", NoticeTone.Success)
            return@SubScreen
        }
        Text(
            "Turning this off means only your password protects this account. Enter your password to confirm.",
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        PasswordInput(password, { password = it }, "Password")
        error?.let { Notice(it) }
        PrimaryButton(text = if (busy) "Turning off…" else "Turn off", enabled = password.isNotEmpty(), busy = busy) {
            busy = true
            error = null
            scope.launch {
                try {
                    account.disableMfa(password)
                    password = ""
                    done = true
                } catch (e: Exception) {
                    error = describeError(e, "Couldn't turn off two-factor authentication.")
                } finally {
                    busy = false
                }
            }
        }
    }
}
