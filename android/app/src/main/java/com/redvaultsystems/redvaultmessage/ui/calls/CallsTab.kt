// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.calls

import androidx.compose.ui.res.stringResource
import com.redvaultsystems.redvaultmessage.ui.components.PlanGate
import android.content.Intent
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.calls.CallLink
import com.redvaultsystems.redvaultmessage.calls.CallsState
import com.redvaultsystems.redvaultmessage.data.CallHistoryStore
import com.redvaultsystems.redvaultmessage.data.CallOutcome
import com.redvaultsystems.redvaultmessage.data.CallRecord
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.ui.components.SectionCaps
import com.redvaultsystems.redvaultmessage.ui.components.TabHeader
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.files.MonoFamily
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale

/** The Calls tab: history (all or missed), call links you host, and joining with a link (a group call's, or someone's call link). */
@Composable
fun CallsTab(
    state: CallsState,
    conversations: List<Conversation>,
    onCall: (conversationId: String, kind: String) -> Unit,
    onNewCall: () -> Unit,
    onCreateLink: (name: String, kind: String) -> Unit,
    onDeleteLink: (String) -> Unit,
    /** Joins from a pasted link or message (a group call's, or someone's call link); [onResult] gets null once the call is starting, or what to show. */
    onJoinLink: (text: String, onResult: (String?) -> Unit) -> Unit,
    onClearHistory: () -> Unit,
    onRefreshLinks: () -> Unit,
    onClearLinkMessages: () -> Unit,
    tier: String,
) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    var missedOnly by rememberSaveable { mutableStateOf(false) }
    var creating by remember { mutableStateOf(false) }
    var joining by remember { mutableStateOf(false) }
    var confirmClear by remember { mutableStateOf(false) }
    var picking by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { onRefreshLinks() }
    if (picking) {
        NewCallDialog(
            contacts = conversations.filter { !it.isGroup }.sortedBy { it.handle.lowercase() },
            available = state.available,
            onDismiss = { picking = false },
            onCall = { id, kind -> picking = false; onCall(id, kind) },
            onAddContacts = { picking = false; onNewCall() },
        )
    }
    val shown = state.history.filter { !missedOnly || it.outcome == CallOutcome.missed }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding()) {
        TabHeader(stringResource(R.string.ui_calls)) {
            RoundButton(R.drawable.ui_link, "Join with a link", onClick = { onClearLinkMessages(); joining = true })
            RoundButton(R.drawable.ui_phone, "New call", onClick = { picking = true }, container = colors.accentFill, tint = colors.accentContrast)
        }
        if (!state.available) {
            Callout(
                stringResource(R.string.ui_calling_isn_t_available_on_this),
                CalloutTone.Neutral,
                R.drawable.ui_info,
                Modifier.padding(horizontal = 20.dp, vertical = 8.dp),
            )
        }
        LazyColumn(Modifier.fillMaxSize()) {
            item {
                Column(Modifier.padding(horizontal = 20.dp).padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                    ChipRow(listOf(false, true), missedOnly, { if (it) "Missed" else "All" }, { missedOnly = it }, edge = 0.dp)
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(16.dp))
                            .background(colors.accentSoft)
                            .border(1.dp, colors.accentBorder, RoundedCornerShape(16.dp))
                            .clickable(role = Role.Button) { onClearLinkMessages(); creating = true }
                            .padding(horizontal = 14.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        Box(Modifier.size(44.dp).clip(RoundedCornerShape(22.dp)).background(colors.accentFill), contentAlignment = Alignment.Center) {
                            DsIcon(R.drawable.ui_link, colors.accentContrast, 20.dp)
                        }
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text(stringResource(R.string.ui_create_a_call_link), color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                            Text(stringResource(R.string.ui_anyone_with_the_link_joins_an), color = colors.text3, fontSize = 13.sp)
                            // Said before the tap: making links is part of Pro (voice) and Premium (video).
                            if (tier == "free") Text("Part of Pro and Premium. Joining a link works on every plan.", color = colors.lockText, fontSize = 12.sp)
                        }
                        if (tier == "free") com.redvaultsystems.redvaultmessage.ui.components.LockBadge()
                    }
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(16.dp))
                            .background(colors.surface2)
                            .clickable(role = Role.Button) { onClearLinkMessages(); joining = true }
                            .padding(horizontal = 14.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        Box(Modifier.size(44.dp).clip(RoundedCornerShape(22.dp)).background(colors.accentSoft), contentAlignment = Alignment.Center) {
                            DsIcon(R.drawable.ui_users, colors.accent, 20.dp)
                        }
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text(stringResource(R.string.ui_join_with_a_link), color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                            Text(stringResource(R.string.ui_a_group_call_s_link_or), color = colors.text3, fontSize = 13.sp)
                        }
                    }
                    state.createdLinkUrl?.let { url ->
                        Callout(stringResource(R.string.ui_link_created_share_it_with_the), CalloutTone.Ok, R.drawable.ui_check) {
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                PillButton(stringResource(R.string.ui_copy), { SecretClipboard.copy(context, url, secret = false) }, style = PillStyle.Secondary, height = 36.dp, icon = R.drawable.ui_copy)
                                PillButton(stringResource(R.string.ui_share), { shareLink(context, url) }, style = PillStyle.Secondary, height = 36.dp, icon = R.drawable.ui_share)
                            }
                        }
                    }
                    state.linkError?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
                }
            }
            if (state.links.isNotEmpty()) {
                item { SectionCaps(stringResource(R.string.ui_your_call_links), Modifier.padding(top = 12.dp)) }
                items(state.links, key = { "link-" + it.id }) { link -> LinkRow(link, onCopy = { SecretClipboard.copy(context, link.url, secret = false) }, onShare = { shareLink(context, link.url) }, onDelete = { onDeleteLink(link.id) }) }
            }
            item {
                Row(Modifier.fillMaxWidth().padding(start = 0.dp, end = 12.dp, top = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                    SectionCaps(stringResource(R.string.ui_recent), Modifier.weight(1f))
                    if (state.history.isNotEmpty()) {
                        Text(
                            stringResource(R.string.ui_clear),
                            color = colors.text3,
                            fontSize = 13.sp,
                            modifier = Modifier.clickable(role = Role.Button) { confirmClear = true }.padding(8.dp),
                        )
                    }
                }
            }
            if (shown.isEmpty()) {
                item {
                    Text(
                        if (missedOnly) "No missed calls." else "No calls yet. Calls are recorded only on this phone, encrypted.",
                        color = colors.text3,
                        fontSize = 14.sp,
                        textAlign = TextAlign.Center,
                        modifier = Modifier.fillMaxWidth().padding(32.dp),
                    )
                }
            }
            items(shown, key = { it.id }) { record ->
                val conversation = record.conversationId?.let { id -> conversations.firstOrNull { it.id == id } }
                HistoryRow(record, conversation, onCall = if (conversation != null && state.available) { kind -> onCall(conversation.id, kind) } else null)
            }
            item { Spacer(Modifier.height(16.dp)) }
        }
    }

    if (creating) {
        CreateLinkDialog(tier, onDismiss = { creating = false }) { name, kind ->
            creating = false
            onCreateLink(name, kind)
        }
    }
    if (joining) {
        var text by remember { mutableStateOf("") }
        var error by remember { mutableStateOf<String?>(null) }
        var busy by remember { mutableStateOf(false) }
        AlertDialog(
            onDismissRequest = { if (!busy) joining = false },
            containerColor = colors.surface2,
            title = { Text(stringResource(R.string.ui_join_with_a_link), color = colors.text) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text(
                        "Paste a call link, or the whole message it came in. A link posted in one of your group chats takes you into that group's call straight away, on any plan. " +
                            "A call link someone sent you asks them to let you in; its part after # stays on this phone and is never sent to the server.",
                        color = colors.text2,
                        fontSize = 14.sp,
                    )
                    DsTextField(text, { text = it; error = null }, "https://…", mono = true, singleLine = false)
                    error?.let { Text(it, color = colors.danger, fontSize = 13.sp) }
                }
            },
            confirmButton = {
                TextButton(enabled = text.isNotBlank() && !busy, onClick = {
                    busy = true
                    onJoinLink(text) { problem ->
                        busy = false
                        if (problem == null) joining = false else error = problem
                    }
                }) { Text(if (busy) "Joining…" else "Join", color = colors.accent) }
            },
            dismissButton = { TextButton(enabled = !busy, onClick = { joining = false }) { Text(stringResource(R.string.ui_cancel), color = colors.text2) } },
        )
    }
    if (confirmClear) {
        AlertDialog(
            onDismissRequest = { confirmClear = false },
            containerColor = colors.surface2,
            title = { Text(stringResource(R.string.ui_clear_call_history), color = colors.text) },
            text = { Text(stringResource(R.string.ui_it_s_only_on_this_phone), color = colors.text2) },
            confirmButton = { TextButton(onClick = { confirmClear = false; onClearHistory() }) { Text(stringResource(R.string.ui_clear), color = colors.danger) } },
            dismissButton = { TextButton(onClick = { confirmClear = false }) { Text(stringResource(R.string.ui_cancel), color = colors.text2) } },
        )
    }
}

@Composable
private fun HistoryRow(record: CallRecord, conversation: Conversation?, onCall: ((String) -> Unit)?) {
    val colors = RedvaultTheme.colors
    val missed = record.outcome == CallOutcome.missed
    val title = record.linkName?.let { "${record.peerHandle} · $it" } ?: record.peerHandle
    val kindWord = if (record.kind == "video") "video" else "voice"
    val detail = buildString {
        append(
            when (record.outcome) {
                CallOutcome.missed -> "Missed $kindWord"
                CallOutcome.declined -> "Declined $kindWord"
                CallOutcome.unanswered -> "No answer"
                CallOutcome.failed -> "Didn't connect"
                CallOutcome.answered -> kindWord.replaceFirstChar { it.uppercase() }
            },
        )
        if (record.durationSec > 0) append(" · ").append(CallHistoryStore.formatDuration(record.durationSec))
        append(" · ").append(whenText(record.startedAt))
    }
    Row(
        Modifier.fillMaxWidth().padding(start = 20.dp, end = 12.dp, top = 9.dp, bottom = 9.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Avatar(conversation?.avatarUrl, record.peerHandle, size = 48.dp)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, color = if (missed) colors.danger else colors.text, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                // Colour is never the only signal: the arrow says which way it went.
                DsIcon(
                    if (record.direction == "incoming") R.drawable.ui_arrow_down_left else R.drawable.ui_arrow_up_right,
                    if (missed) colors.danger else colors.text3,
                    14.dp,
                    description = if (record.direction == "incoming") "Incoming" else "Outgoing",
                )
                Text(detail, color = if (missed) colors.danger else colors.text3, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        if (onCall != null) {
            RoundButton(
                if (record.kind == "video") R.drawable.ui_video else R.drawable.ui_phone,
                if (record.kind == "video") "Video call ${record.peerHandle}" else "Call ${record.peerHandle}",
                onClick = { onCall(record.kind) },
                container = Color.Transparent,
            )
        }
    }
}

@Composable
private fun LinkRow(link: CallLink, onCopy: () -> Unit, onShare: () -> Unit, onDelete: () -> Unit) {
    val colors = RedvaultTheme.colors
    var confirm by remember { mutableStateOf(false) }
    Row(
        Modifier.fillMaxWidth().padding(start = 20.dp, end = 12.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Box(Modifier.size(44.dp).clip(RoundedCornerShape(12.dp)).background(colors.surface2), contentAlignment = Alignment.Center) {
            DsIcon(if (link.kind == "video") R.drawable.ui_video else R.drawable.ui_phone, colors.accent, 20.dp)
        }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(link.name, color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(if (link.kind == "video") "Video link" else "Voice link", color = colors.text3, fontFamily = MonoFamily, fontSize = 12.sp)
        }
        RoundButton(R.drawable.ui_copy, "Copy ${link.name}", onClick = onCopy, size = 40.dp, iconSize = 17.dp)
        RoundButton(R.drawable.ui_share, "Share ${link.name}", onClick = onShare, size = 40.dp, iconSize = 17.dp)
        RoundButton(R.drawable.ui_trash, "Delete ${link.name}", onClick = { confirm = true }, size = 40.dp, iconSize = 17.dp)
    }
    if (confirm) {
        AlertDialog(
            onDismissRequest = { confirm = false },
            containerColor = colors.surface2,
            title = { Text("Delete “${link.name}”?", color = colors.text) },
            text = { Text(stringResource(R.string.ui_the_link_stops_working_for_everyone), color = colors.text2) },
            confirmButton = { TextButton(onClick = { confirm = false; onDelete() }) { Text(stringResource(R.string.ui_delete), color = colors.danger) } },
            dismissButton = { TextButton(onClick = { confirm = false }) { Text(stringResource(R.string.ui_cancel), color = colors.text2) } },
        )
    }
}

@Composable
private fun CreateLinkDialog(tier: String, onDismiss: () -> Unit, onCreate: (String, String) -> Unit) {
    val colors = RedvaultTheme.colors
    var name by remember { mutableStateOf("") }
    var video by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface2,
        title = { Text(stringResource(R.string.ui_create_a_call_link), color = colors.text) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    stringResource(R.string.ui_anyone_with_the_link_can_ask),
                    color = colors.text2,
                    fontSize = 14.sp,
                )
                DsTextField(name, { if (it.length <= 60) name = it }, "Name, e.g. Weekly catch-up")
                ChipRow(
                    listOf(false, true), video, { if (it) "Video" else "Voice" }, { video = it },
                    // Video links are Premium: the chip shows a lock on other plans.
                    lockOption = if (tier != "premium") true else null,
                    lockIcon = if (tier != "premium") R.drawable.ui_lock else null,
                    edge = 0.dp,
                )
                Text(
                    when {
                        video && tier != "premium" -> "Video call links are part of Premium."
                        !video && tier == "free" -> "Call links are part of Pro (voice) and Premium (video). Anyone can join a link on any plan."
                        else -> "You host: the call uses your plan."
                    },
                    color = colors.text3,
                    fontSize = 13.sp,
                )
            }
        },
        confirmButton = {
            // Locked rather than refused afterwards: Create explains the plan when it doesn't include this kind of link.
            val allowed = if (video) tier == "premium" else tier == "pro" || tier == "premium"
            TextButton(onClick = {
                if (allowed) onCreate(name, if (video) "video" else "voice")
                else {
                    onDismiss()
                    PlanGate.ask(if (video) "premium" else "pro", if (video) "A video call link" else "A call link")
                }
            }) { Text(if (allowed) "Create" else "See plans", color = colors.accent) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.ui_cancel), color = colors.text2) } },
    )
}

private fun shareLink(context: android.content.Context, url: String) {
    val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, url)
    runCatching { context.startActivity(Intent.createChooser(send, "Share call link")) }
}

private fun whenText(ms: Long): String {
    val day = Calendar.getInstance().apply { timeInMillis = ms }
    val today = Calendar.getInstance()
    val yesterday = Calendar.getInstance().apply { add(Calendar.DAY_OF_YEAR, -1) }
    fun same(a: Calendar, b: Calendar) = a.get(Calendar.YEAR) == b.get(Calendar.YEAR) && a.get(Calendar.DAY_OF_YEAR) == b.get(Calendar.DAY_OF_YEAR)
    return when {
        same(day, today) -> SimpleDateFormat("HH:mm", Locale.getDefault()).format(Date(ms))
        same(day, yesterday) -> "Yesterday"
        today.timeInMillis - ms < 6 * 24 * 3600 * 1000L -> SimpleDateFormat("EEE", Locale.getDefault()).format(Date(ms))
        else -> SimpleDateFormat("MMM d", Locale.getDefault()).format(Date(ms))
    }
}

/** "New call": pick a contact, then voice or video. With no contacts yet it offers the Contacts tab. */
@Composable
private fun NewCallDialog(
    contacts: List<Conversation>,
    available: Boolean,
    onDismiss: () -> Unit,
    onCall: (conversationId: String, kind: String) -> Unit,
    onAddContacts: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    var query by remember { mutableStateOf("") }
    val shown = contacts.filter { query.isBlank() || it.handle.contains(query.trim(), ignoreCase = true) }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface2,
        title = { Text(stringResource(R.string.ui_new_call), color = colors.text) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                when {
                    !available -> Text(stringResource(R.string.ui_calling_isn_t_available_on_this), color = colors.text2, fontSize = 14.sp)
                    contacts.isEmpty() -> Text(stringResource(R.string.ui_add_a_contact_first_you_can), color = colors.text2, fontSize = 14.sp)
                    else -> {
                        DsTextField(query, { query = it }, "Search contacts")
                        LazyColumn(Modifier.fillMaxWidth().height(320.dp)) {
                            items(shown, key = { it.id }) { contact ->
                                Row(
                                    Modifier.fillMaxWidth().padding(vertical = 6.dp),
                                    verticalAlignment = Alignment.CenterVertically,
                                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                                ) {
                                    Avatar(avatarUrl = contact.avatarUrl, seed = contact.handle, size = 36.dp)
                                    Text(contact.handle, color = colors.text, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                                    RoundButton(R.drawable.ui_phone, "Voice call ${contact.handle}", onClick = { onCall(contact.id, "voice") })
                                    RoundButton(R.drawable.ui_video, "Video call ${contact.handle}", onClick = { onCall(contact.id, "video") })
                                }
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {
            if (contacts.isEmpty() && available) TextButton(onClick = onAddContacts) { Text(stringResource(R.string.ui_go_to_contacts), color = colors.accent) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.ui_cancel), color = colors.text2) } },
    )
}
