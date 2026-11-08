// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.calls

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
    LaunchedEffect(Unit) { onRefreshLinks() }
    val shown = state.history.filter { !missedOnly || it.outcome == CallOutcome.missed }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding()) {
        TabHeader("Calls") {
            RoundButton(R.drawable.ui_link, "Join with a link", onClick = { onClearLinkMessages(); joining = true })
            RoundButton(R.drawable.ui_phone, "New call", onClick = onNewCall, container = colors.accent, tint = colors.accentContrast)
        }
        if (!state.available) {
            Callout(
                "Calling isn't available on this server right now.",
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
                            .background(Color(0xFF16271F))
                            .border(1.dp, Color(0xFF244536), RoundedCornerShape(16.dp))
                            .clickable(role = Role.Button) { onClearLinkMessages(); creating = true }
                            .padding(horizontal = 14.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        Box(Modifier.size(44.dp).clip(RoundedCornerShape(22.dp)).background(colors.accent), contentAlignment = Alignment.Center) {
                            DsIcon(R.drawable.ui_link, colors.accentContrast, 20.dp)
                        }
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text("Create a call link", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                            Text("Anyone with the link joins an encrypted call", color = colors.text3, fontSize = 13.sp)
                        }
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
                        Box(Modifier.size(44.dp).clip(RoundedCornerShape(22.dp)).background(Color(0xFF16271F)), contentAlignment = Alignment.Center) {
                            DsIcon(R.drawable.ui_users, colors.accent, 20.dp)
                        }
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text("Join with a link", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                            Text("A group call's link, or a call link someone sent you", color = colors.text3, fontSize = 13.sp)
                        }
                    }
                    state.createdLinkUrl?.let { url ->
                        Callout("Link created. Share it with the people you want to call; you'll ring when someone opens it.", CalloutTone.Ok, R.drawable.ui_check) {
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                PillButton("Copy", { SecretClipboard.copy(context, url, secret = false) }, style = PillStyle.Secondary, height = 36.dp, icon = R.drawable.ui_copy)
                                PillButton("Share", { shareLink(context, url) }, style = PillStyle.Secondary, height = 36.dp, icon = R.drawable.ui_share)
                            }
                        }
                    }
                    state.linkError?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
                }
            }
            if (state.links.isNotEmpty()) {
                item { SectionCaps("Your call links", Modifier.padding(top = 12.dp)) }
                items(state.links, key = { "link-" + it.id }) { link -> LinkRow(link, onCopy = { SecretClipboard.copy(context, link.url, secret = false) }, onShare = { shareLink(context, link.url) }, onDelete = { onDeleteLink(link.id) }) }
            }
            item {
                Row(Modifier.fillMaxWidth().padding(start = 0.dp, end = 12.dp, top = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                    SectionCaps("Recent", Modifier.weight(1f))
                    if (state.history.isNotEmpty()) {
                        Text(
                            "Clear",
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
            title = { Text("Join with a link", color = colors.text) },
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
            dismissButton = { TextButton(enabled = !busy, onClick = { joining = false }) { Text("Cancel", color = colors.text2) } },
        )
    }
    if (confirmClear) {
        AlertDialog(
            onDismissRequest = { confirmClear = false },
            containerColor = colors.surface2,
            title = { Text("Clear call history?", color = colors.text) },
            text = { Text("It's only on this phone, so it's gone for good.", color = colors.text2) },
            confirmButton = { TextButton(onClick = { confirmClear = false; onClearHistory() }) { Text("Clear", color = colors.danger) } },
            dismissButton = { TextButton(onClick = { confirmClear = false }) { Text("Cancel", color = colors.text2) } },
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
            text = { Text("The link stops working for everyone who has it.", color = colors.text2) },
            confirmButton = { TextButton(onClick = { confirm = false; onDelete() }) { Text("Delete", color = colors.danger) } },
            dismissButton = { TextButton(onClick = { confirm = false }) { Text("Cancel", color = colors.text2) } },
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
        title = { Text("Create a call link", color = colors.text) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    "Anyone with the link can ask to join; you'll ring and decide. The name and the call are encrypted with a secret that's only in the link.",
                    color = colors.text2,
                    fontSize = 14.sp,
                )
                DsTextField(name, { if (it.length <= 60) name = it }, "Name, e.g. Weekly catch-up")
                ChipRow(listOf(false, true), video, { if (it) "Video" else "Voice" }, { video = it }, edge = 0.dp)
                Text(
                    when {
                        video && tier != "premium" -> "Video call links are part of Premium."
                        !video && tier == "free" -> "Call links are part of Pro and Premium."
                        else -> "You host: the call uses your plan."
                    },
                    color = colors.text3,
                    fontSize = 13.sp,
                )
            }
        },
        confirmButton = { TextButton(onClick = { onCreate(name, if (video) "video" else "voice") }) { Text("Create", color = colors.accent) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel", color = colors.text2) } },
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
