// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.settings

import androidx.compose.ui.res.stringResource
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
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
    /** In-app updates: the Updates section above "Open source" (update/AppUpdates.kt). */
    updates: com.redvaultsystems.redvaultmessage.update.AppUpdates? = null,
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
    /** Devices & encryption (per-device keys); null hides the row (a build without the engine). */
    onOpenDeviceKeys: (() -> Unit)? = null,
    onOpenRelayServers: () -> Unit = {},
    onOpenTransparencyLog: () -> Unit = {},
    ringtone: Boolean = true,
    onRingtoneChange: (Boolean) -> Unit = {},
    echoCancellation: Boolean = true,
    onEchoCancellationChange: (Boolean) -> Unit = {},
    /** Story settings; null while the server has Stories off (then there is nothing to show). */
    onOpenStorySettings: (() -> Unit)? = null,
    /** Settings › Plans: compare plans, buy, switch or cancel. */
    onOpenPlans: () -> Unit = {},
    /** Files (the encrypted drive) in use and its limit, in bytes; null until it has loaded. Every plan has some. */
    filesUsed: Long? = null,
    filesLimit: Long? = null,
    /** Settings › Appearance: "system", "dark" or "light" (AppearanceStore). */
    appearance: String = "dark",
    onAppearanceChange: (String) -> Unit = {},
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
    SubScreen(title = stringResource(R.string.ui_settings), onBack = onBack) {
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
                            stringResource(R.string.ui_remove),
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
        Text(stringResource(R.string.ui_your_photo_is_visible_to_every), color = colors.text3, fontSize = 12.5.sp)
        error?.let { Notice(it) }

        SectionLabel(stringResource(R.string.ui_privacy))
        SettingsRow(
            title = stringResource(R.string.ui_maximum_privacy),
            description = stringResource(R.string.ui_see_and_turn_on_every_privacy),
            onClick = onOpenPrivacyMode,
            icon = R.drawable.ui_shield_check,
        )
        SettingsRow(
            title = stringResource(R.string.ui_network_routing),
            description = if (torRouting) "Messages, files and account: through Tor. Calls: encrypted relay (not Tor)"
            else "How messages, files and calls leave this phone",
            onClick = onOpenNetworkRouting,
            icon = R.drawable.ui_globe,
        )
        AppLockRow(appLockEnabled, appLockTimeout, onOpenAppLock)
        SettingsRow(
            title = stringResource(R.string.ui_read_receipts),
            description = stringResource(R.string.ui_let_contacts_see_when_you_ve),
            icon = R.drawable.ui_check,
            trailing = { RvSwitch(checked = readReceipts, onCheckedChange = onReadReceiptsChange, description = stringResource(R.string.ui_read_receipts)) },
        )
        SettingsRow(
            title = stringResource(R.string.ui_show_when_i_m_online),
            description = stringResource(R.string.ui_contacts_who_share_theirs_see_when),
            icon = R.drawable.ui_signal,
            trailing = {
                RvSwitch(
                    checked = profile.showOnlineStatus == true,
                    enabled = profile.showOnlineStatus != null,
                    onCheckedChange = { on -> run("Couldn't change that. Try again.") { account.setShowOnlineStatus(on) } },
                    description = stringResource(R.string.ui_show_when_i_m_online),
                )
            },
        )
        SettingsRow(
            title = stringResource(R.string.ui_typing_indicators),
            description = stringResource(R.string.ui_show_when_you_re_typing_and),
            icon = R.drawable.ui_dots,
            trailing = { RvSwitch(checked = typingIndicators, onCheckedChange = onTypingIndicatorsChange, description = stringResource(R.string.ui_typing_indicators)) },
        )
        SettingsRow(
            title = stringResource(R.string.ui_hide_app_content),
            description = stringResource(R.string.ui_block_screenshots_and_hide_chats_in),
            icon = R.drawable.ui_eye_off,
            trailing = { RvSwitch(checked = hideContent, onCheckedChange = onHideContentChange, description = stringResource(R.string.ui_hide_app_content)) },
        )

        SectionLabel(stringResource(R.string.ui_appearance))
        Column(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(stringResource(R.string.ui_theme), color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(horizontal = 14.dp))
            ChipRow(
                listOf("system", "dark", "light"),
                appearance,
                { when (it) { "system" -> "Match phone"; "light" -> "Light"; else -> "Dark" } },
                onAppearanceChange,
                edge = 14.dp,
            )
            Text(stringResource(R.string.ui_calls_stay_dark_in_every_theme), color = colors.text3, fontSize = 12.5.sp, modifier = Modifier.padding(horizontal = 14.dp))
        }

        SectionLabel(stringResource(R.string.ui_notifications_and_calls))
        SettingsRow(
            title = stringResource(R.string.ui_notifications),
            description = stringResource(R.string.ui_tell_me_when_something_new_arrives),
            icon = R.drawable.ui_bell,
            trailing = { RvSwitch(checked = notificationsOn, onCheckedChange = onNotificationsChange, description = stringResource(R.string.ui_notifications)) },
        )
        if (notificationsOn) {
            SettingsRow(
                title = stringResource(R.string.ui_how_they_arrive),
                description = when (pushMode) {
                    is PushMode.UnifiedPush -> if (pushMode.distributor.isEmpty()) "Push through your push app" else "Push through ${distributorName(pushMode.distributor)}"
                    PushMode.BackgroundConnection -> if (torRouting) "Background connection, through Tor" else "Background connection"
                    PushMode.Off -> "Off"
                },
                icon = R.drawable.ui_info,
            )
            // Through Tor there's no push app at all (docs/tor-by-default.md rule 6): the background connection is the only way.
            val backgroundTitle = if (torRouting) "Background connection" else "Background connection when no push app is installed"
            SettingsRow(
                title = backgroundTitle,
                description = if (torRouting) {
                    "Keeps a connection to the server open through Tor, with a quiet ongoing notification. No push service run by anyone else is used"
                } else {
                    "Keeps a connection to the server open, with a quiet ongoing notification. Uses more battery than a push app"
                },
                icon = R.drawable.ui_bell,
                trailing = {
                    RvSwitch(checked = backgroundConnection, onCheckedChange = onBackgroundConnectionChange, description = backgroundTitle)
                },
            )
            if (torRouting) {
                // No UnifiedPush app is used through Tor: nothing to install or choose.
            } else if (distributorCount == 0) {
                Text(
                    stringResource(R.string.ui_installing_a_unifiedpush_app_such_as),
                    color = colors.text3, fontSize = 12.5.sp,
                )
            } else if (distributorCount > 1) {
                SettingsRow(
                    title = stringResource(R.string.ui_choose_push_app),
                    description = "$distributorCount UnifiedPush apps are installed",
                    onClick = onPickDistributor,
                    icon = R.drawable.ui_bell,
                )
            }
        }
        SettingsRow(
            title = stringResource(R.string.ui_ringtone),
            description = stringResource(R.string.ui_ring_for_incoming_calls_muted_contacts),
            icon = R.drawable.ui_phone,
            trailing = { RvSwitch(checked = ringtone, onCheckedChange = onRingtoneChange, description = stringResource(R.string.ui_ringtone)) },
        )
        SettingsRow(
            title = stringResource(R.string.ui_echo_cancellation),
            description = stringResource(R.string.ui_stops_the_other_person_hearing_themselves),
            icon = R.drawable.ui_headphones,
            trailing = { RvSwitch(checked = echoCancellation, onCheckedChange = onEchoCancellationChange, description = stringResource(R.string.ui_echo_cancellation)) },
        )

        SectionLabel(stringResource(R.string.ui_friends))
        SettingsRow(
            title = stringResource(R.string.ui_accept_messages_from_anyone),
            description = stringResource(R.string.ui_off_rejects_new_friend_requests_from),
            icon = R.drawable.ui_user_plus,
            trailing = {
                RvSwitch(
                    checked = profile.acceptMessageRequests ?: true,
                    enabled = profile.acceptMessageRequests != null,
                    onCheckedChange = { run("Couldn't update that setting. Try again.") { account.setAcceptMessageRequests(it) } },
                    description = stringResource(R.string.ui_accept_messages_from_anyone),
                )
            },
        )
        SettingsRow(
            title = stringResource(R.string.ui_friend_requests),
            description = if (incomingRequests > 0) "$incomingRequests waiting for you" else "Review, accept or deny requests",
            onClick = onOpenFriendRequests,
            icon = R.drawable.ui_users,
        )
        SettingsRow(
            title = stringResource(R.string.ui_blocked_users),
            description = if (blockedCount == 0) "Nobody blocked" else "$blockedCount blocked",
            onClick = onOpenBlocked,
            icon = R.drawable.ui_ban,
        )
        SettingsRow(
            title = stringResource(R.string.ui_inbox),
            description = if (inboxUnread > 0) "$inboxUnread unread from the RedVault team" else "Messages and polls from the RedVault team",
            onClick = onOpenInbox,
            icon = R.drawable.ui_bell,
        )

        if (onOpenStorySettings != null) {
            SettingsRow(title = stringResource(R.string.ui_stories), description = stringResource(R.string.ui_who_sees_your_stories_archive_and), onClick = onOpenStorySettings, icon = R.drawable.ui_image)
        }

        SectionLabel(stringResource(R.string.ui_account))
        SettingsRow(title = stringResource(R.string.ui_handle), description = profile.handle, onClick = onOpenChangeUsername, icon = R.drawable.ui_user)
        PlanRow(account, profile.subscriptionTier, onOpenPlans)
        if (filesUsed != null && filesLimit != null && filesLimit > 0) FilesStorageRow(filesUsed, filesLimit, profile.subscriptionTier, onOpenPlans)
        StorageRow(account)

        SectionLabel(stringResource(R.string.ui_security))
        SettingsRow(
            title = stringResource(R.string.ui_recovery_key),
            description = when (profile.hasRecoveryKey) {
                false -> "Not set up: a forgotten password can't be recovered"
                else -> "The only way back in if you forget your password"
            },
            onClick = onOpenRecoveryKey,
            icon = R.drawable.ui_key,
        )
        SettingsRow(title = stringResource(R.string.ui_change_password), description = stringResource(R.string.ui_re_encrypts_your_private_key_with), onClick = onOpenChangePassword, icon = R.drawable.ui_lock)
        SettingsRow(
            title = stringResource(R.string.ui_two_factor_authentication),
            description = if (profile.mfaEnabled) "On. Tap to turn off" else "Off. Set up an authenticator app",
            onClick = onOpenMfa,
            icon = R.drawable.ui_shield,
        )
        SettingsRow(title = stringResource(R.string.ui_linked_devices), description = stringResource(R.string.ui_everywhere_you_re_signed_in_sign), onClick = onOpenLinkedDevices, icon = R.drawable.ui_monitor)
        if (onOpenDeviceKeys != null) {
            SettingsRow(title = stringResource(R.string.ui_devices_encryption), description = stringResource(R.string.ui_the_signal_protocol_your_devices_and), onClick = onOpenDeviceKeys, icon = R.drawable.ui_shield)
        }

        SectionLabel(stringResource(R.string.ui_network))
        SettingsRow(title = stringResource(R.string.ui_relay_servers), description = stringResource(R.string.ui_the_message_and_call_relays_and), onClick = onOpenRelayServers, icon = R.drawable.ui_globe)
        SettingsRow(
            title = stringResource(R.string.ui_transparency_log),
            description = stringResource(R.string.ui_check_on_this_phone_that_the),
            onClick = onOpenTransparencyLog,
            icon = R.drawable.ui_log,
        )
        SettingsRow(title = stringResource(R.string.ui_live_transparency_feed), description = stringResource(R.string.ui_the_public_record_of_relayed_messages), onClick = onOpenTransparency, icon = R.drawable.ui_signal)
        SettingsRow(
            title = stringResource(R.string.ui_transparency_mode),
            description = stringResource(R.string.ui_when_on_your_outgoing_messages_ciphertext),
            icon = R.drawable.ui_eye,
            trailing = {
                RvSwitch(
                    checked = profile.transparencyMode ?: false,
                    enabled = profile.transparencyMode != null,
                    onCheckedChange = { run("Couldn't update that setting. Try again.") { account.setTransparencyMode(it) } },
                    description = stringResource(R.string.ui_transparency_mode),
                )
            },
        )

        UpdateSection(updates)

        OpenSourceSection()

        Spacer(Modifier.height(8.dp))
        PillButton(stringResource(R.string.ui_log_out), onLogout, Modifier.fillMaxWidth(), style = PillStyle.Secondary, icon = R.drawable.ui_log_out)
        PillButton(stringResource(R.string.ui_delete_account), onOpenDeleteAccount, Modifier.fillMaxWidth(), style = PillStyle.Quiet)
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

    SubScreen(title = stringResource(R.string.ui_delete_account), onBack = onBack) {
        Text(
            "This permanently deletes your account and signs you out on every device. Your private key, recovery key, " +
                "two-factor setup, profile photo and notification tokens are removed. You can't undo this, and your " +
                "username can't be used again.",
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        Text(
            stringResource(R.string.ui_messages_you_already_sent_stay_in),
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        PasswordInput(password, { password = it }, "Password")
        Row(verticalAlignment = Alignment.CenterVertically) {
            Checkbox(checked = confirmed, onCheckedChange = { confirmed = it })
            Text(stringResource(R.string.ui_i_understand_this_can_t_be), color = RedvaultTheme.colors.text, style = MaterialTheme.typography.bodyMedium)
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
 * The plan and where it's headed; tapping opens Plans, where you buy,
 * switch, cancel or keep a plan (PlansScreen).
 */
@Composable
private fun PlanRow(account: AccountSession, tier: String, onOpenPlans: () -> Unit) {
    var status by remember { mutableStateOf<PlanStatusDto?>(null) }
    // An older server has no /billing/status; the row then shows the tier alone.
    LaunchedEffect(tier) { status = runCatching { account.planStatus() }.getOrNull() }
    val current = status
    val plan = if (tier == "premium") "Premium" else "Pro"
    val description = when {
        !isPaidTier(tier) -> "Free. See what Pro and Premium add"
        current?.endsOn != null ->
            "$plan until ${formatDay(current.endsOn)}, then Free" + if (current.source == "crypto") " (prepaid, nothing renews)" else ""
        current?.renews == true -> "$plan · renews monthly"
        else -> plan
    }
    SettingsRow(title = stringResource(R.string.ui_plan), description = description, onClick = onOpenPlans, icon = R.drawable.ui_star)
}

/** Files (the encrypted drive): every plan has some storage, so this shows on Free too. */
@Composable
private fun FilesStorageRow(used: Long, limit: Long, tier: String, onOpenPlans: () -> Unit) {
    val colors = RedvaultTheme.colors
    val fraction = (used.toFloat() / limit).coerceIn(0f, 1f)
    Column(
        Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(12.dp)).padding(horizontal = 16.dp, vertical = 14.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text(stringResource(R.string.ui_files_storage), color = colors.text, fontWeight = FontWeight.Medium)
        Text(
            "${FileMessage.formatBytes(used)} of ${FileMessage.formatBytes(limit)} used (${PlanChoices.nameOf(tier)})",
            color = colors.text2,
            style = MaterialTheme.typography.bodySmall,
        )
        LinearProgressIndicator(
            progress = { fraction },
            color = if (fraction >= 1f) colors.danger else if (fraction >= 0.9f) colors.warn else colors.accent,
            trackColor = colors.surface2,
            modifier = Modifier.fillMaxWidth(),
        )
        if (fraction >= 0.9f && tier != "premium") {
            Text(
                if (tier == "pro") "Get 100 GB with Premium" else "Get more space",
                color = colors.accent,
                fontWeight = FontWeight.SemiBold,
                fontSize = 14.sp,
                modifier = Modifier.clickable(role = Role.Button, onClick = onOpenPlans).padding(vertical = 4.dp),
            )
        }
    }
}

/** Pro and Premium: storage used by files you've sent in chats. Hidden if file sharing isn't set up on the server. */
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
        Text(stringResource(R.string.ui_files_sent_in_chats), color = colors.text, fontWeight = FontWeight.Medium)
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

    SubScreen(title = stringResource(R.string.ui_change_handle), onBack = onBack) {
        Text(
            stringResource(R.string.ui_your_friends_see_the_new_handle),
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        DsTextField(
            username,
            { if (it.length <= MAX_USERNAME_LENGTH) username = it },
            "Handle",
            mono = true,
            keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false),
        )
        error?.let { Notice(it) }
        if (done) Notice("Your handle is now $current.", NoticeTone.Success)
        PrimaryButton(text = stringResource(R.string.ui_save), enabled = valid, busy = busy) {
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

    SubScreen(title = stringResource(R.string.ui_change_password), onBack = onBack) {
        Text(
            "Your private key is re-encrypted with the new password on this phone before anything is sent. " +
                "Other devices are signed out. There's still no way to recover a forgotten password.",
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        PasswordInput(current, { current = it }, "Current password")
        PasswordInput(next, { next = it }, "New password (min. $MIN_PASSWORD_LENGTH characters)")
        PasswordInput(confirm, { confirm = it }, "Confirm new password")
        if (confirm.isNotEmpty() && confirm != next) Notice(stringResource(R.string.ui_new_passwords_don_t_match))
        error?.let { Notice(it) }
        if (done) Notice(stringResource(R.string.ui_password_changed), NoticeTone.Success)
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

    SubScreen(title = stringResource(R.string.ui_two_factor_authentication), onBack = onBack) {
        if (done) {
            Notice(stringResource(R.string.ui_two_factor_authentication_is_on_you), NoticeTone.Success)
            return@SubScreen
        }
        Text(
            stringResource(R.string.ui_add_this_account_to_an_authenticator),
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        error?.let { Notice(it) }
        val current = setup
        if (current == null && error == null) Text(stringResource(R.string.ui_loading), color = RedvaultTheme.colors.text3)
        if (current != null) {
            // On the phone itself there's no QR to scan: hand the otpauth://
            // link straight to an installed authenticator app instead.
            PrimaryButton(text = stringResource(R.string.ui_open_in_authenticator_app), enabled = true) {
                try {
                    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(current.otpauthUri)))
                } catch (_: ActivityNotFoundException) {
                    error = "No authenticator app found. Install one, or enter the key below by hand."
                }
            }
            Text(stringResource(R.string.ui_or_enter_this_key_by_hand), color = RedvaultTheme.colors.text2, style = MaterialTheme.typography.bodyMedium)
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

            DsTextField(
                code,
                { code = it.filter(Char::isDigit).take(6) },
                "6-digit code",
                mono = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
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

    SubScreen(title = stringResource(R.string.ui_two_factor_authentication), onBack = onBack) {
        if (done) {
            Notice(stringResource(R.string.ui_two_factor_authentication_is_off), NoticeTone.Success)
            return@SubScreen
        }
        Text(
            stringResource(R.string.ui_turning_this_off_means_only_your),
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
