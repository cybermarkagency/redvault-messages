package com.redvaultsystems.redvaultmessage.ui.contacts

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.compose.foundation.Image
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
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.data.model.Disappearing
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.Callout
import com.redvaultsystems.redvaultmessage.ui.components.CalloutTone
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.components.SearchBox
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.ui.components.SectionCaps
import com.redvaultsystems.redvaultmessage.ui.components.TabHeader
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.files.MonoFamily
import com.redvaultsystems.redvaultmessage.ui.live.LiveState
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import com.redvaultsystems.redvaultmessage.ui.verify.qrBitmap
import kotlinx.coroutines.launch

/** Everyone you have a one-to-one chat with, as the Contacts tab lists them. */
fun contactsOf(conversations: List<Conversation>): List<Conversation> =
    conversations.filter { !it.isGroup && it.meta.otherUserId.isNotEmpty() }.sortedBy { it.handle.lowercase() }

/** What a scanned contact code must look like: a username (anything else, like a safety-number code, is refused). */
private val HANDLE_RE = Regex("^[A-Za-z0-9._-]{2,40}$")

/** The Contacts tab: scan, add, your contact code, favorites, then everyone A–Z. */
@Composable
fun ContactsTab(
    conversations: List<Conversation>,
    live: LiveState,
    onOpen: (userId: String) -> Unit,
    onAdd: (handle: String) -> Unit,
    onShareCode: () -> Unit,
    /** Voice or video call a contact (by chat id); null when calling isn't available. */
    onCall: ((conversationId: String, kind: String) -> Unit)? = null,
) {
    val colors = RedvaultTheme.colors
    var search by rememberSaveable { mutableStateOf("") }
    var scanError by remember { mutableStateOf<String?>(null) }
    val scan = rememberLauncherForActivityResult(ScanContract()) { result ->
        val contents = result.contents?.trim() ?: return@rememberLauncherForActivityResult
        if (HANDLE_RE.matches(contents)) {
            scanError = null
            onAdd(contents)
        } else {
            scanError = "That isn't a contact code. Scan the code on their Contacts screen."
        }
    }
    val contacts = contactsOf(conversations)
    val q = search.trim().lowercase()
    val shown = contacts.filter { q.isEmpty() || it.handle.lowercase().contains(q) }
    val favorites = contacts.filter { it.meta.otherUserId in live.favorites }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding()) {
        TabHeader("Contacts") {
            RoundButton(R.drawable.ui_qr, "Scan contact code", onClick = {
                scan.launch(
                    ScanOptions().setDesiredBarcodeFormats(ScanOptions.QR_CODE).setPrompt("Scan their contact code").setBeepEnabled(false).setOrientationLocked(false),
                )
            })
            RoundButton(R.drawable.ui_user_plus, "Add contact", onClick = { onAdd("") }, container = colors.accent, tint = colors.accentContrast)
        }
        LazyColumn(Modifier.fillMaxSize()) {
            item {
                Column(Modifier.padding(horizontal = 20.dp).padding(top = 8.dp, bottom = 6.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                    SearchBox(search, { search = it }, "Search name or username", R.drawable.ui_search)
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(16.dp))
                            .background(Color(0xFF16271F))
                            .border(1.dp, Color(0xFF244536), RoundedCornerShape(16.dp))
                            .clickable(role = Role.Button, onClick = onShareCode)
                            .padding(horizontal = 14.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        Box(Modifier.size(44.dp).clip(RoundedCornerShape(22.dp)).background(colors.accent), contentAlignment = Alignment.Center) {
                            DsIcon(R.drawable.ui_qr, colors.accentContrast, 20.dp)
                        }
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text("Share your contact code", color = colors.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                            Text("No phone number needed to connect", color = colors.text3, fontSize = 13.sp)
                        }
                    }
                    scanError?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
                }
            }
            if (favorites.isNotEmpty() && q.isEmpty()) {
                item { SectionCaps("Favorites", Modifier.padding(top = 4.dp)) }
                item {
                    LazyRow(Modifier.padding(bottom = 6.dp), contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 20.dp), horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                        items(favorites, key = { it.id }) { c ->
                            Column(
                                Modifier.width(62.dp).clickable(role = Role.Button) { onOpen(c.meta.otherUserId) },
                                horizontalAlignment = Alignment.CenterHorizontally,
                                verticalArrangement = Arrangement.spacedBy(6.dp),
                            ) {
                                Avatar(c.avatarUrl, c.handle, size = 58.dp, online = c.meta.otherUserId in live.online)
                                Text(c.handle, color = colors.text, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            }
                        }
                    }
                }
            }
            if (contacts.isEmpty()) {
                item {
                    Text(
                        "No contacts yet. Add someone by their username, or share your contact code.",
                        color = colors.text3,
                        fontSize = 14.sp,
                        textAlign = TextAlign.Center,
                        modifier = Modifier.fillMaxWidth().padding(32.dp),
                    )
                }
            } else if (shown.isEmpty()) {
                item { Text("Nobody matches that.", color = colors.text3, fontSize = 14.sp, modifier = Modifier.padding(20.dp)) }
            }
            // A–Z sections.
            shown.groupBy { it.handle.firstOrNull()?.uppercaseChar()?.takeIf(Char::isLetter) ?: '#' }.toSortedMap().forEach { (letter, group) ->
                item(key = "section-$letter") {
                    Text(
                        letter.toString(),
                        color = colors.accent,
                        fontFamily = MonoFamily,
                        fontSize = 13.sp,
                        fontWeight = FontWeight.Medium,
                        modifier = Modifier.padding(start = 20.dp, top = 8.dp, bottom = 4.dp),
                    )
                }
                items(group, key = { it.id }) { c -> ContactRow(c, live, onCall?.let { call -> { kind: String -> call(c.id, kind) } }) { onOpen(c.meta.otherUserId) } }
            }
            item { Spacer(Modifier.height(12.dp)) }
        }
    }
}

@Composable
private fun ContactRow(c: Conversation, live: LiveState, onCall: ((String) -> Unit)?, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    val userId = c.meta.otherUserId
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(start = 20.dp, end = 16.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Avatar(c.avatarUrl, c.handle, size = 44.dp, online = userId in live.online)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(c.handle, color = colors.text, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            when {
                c.keyChanged -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    DsIcon(R.drawable.ic_alert, colors.warn, 13.dp)
                    Text("Safety number changed", color = colors.warn, fontSize = 12.sp)
                }
                userId in live.online -> Text("online", color = colors.accent, fontSize = 12.sp)
                c.verified -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    DsIcon(R.drawable.ui_shield_check, colors.accent, 13.dp)
                    Text("Verified", color = colors.accent, fontSize = 12.sp)
                }
                else -> Text("@${c.handle}", color = colors.text3, fontFamily = MonoFamily, fontSize = 12.sp, maxLines = 1)
            }
        }
        if (userId in live.muted) DsIcon(R.drawable.ui_bell_off, colors.text3, 18.dp, description = "Muted")
        if (userId in live.favorites) DsIcon(R.drawable.ui_star, colors.warn, 18.dp, description = "Favorite")
        if (onCall != null) {
            RoundButton(R.drawable.ui_phone, "Call ${c.handle}", onClick = { onCall("voice") }, container = Color.Transparent)
            RoundButton(R.drawable.ui_video, "Video call ${c.handle}", onClick = { onCall("video") }, container = Color.Transparent)
        }
    }
}

// --- One contact ---

/** One contact: message, favorite, mute, safety number, disappearing messages, shared files, groups in common, block and report. */
@Composable
fun ContactScreen(
    conversation: Conversation?,
    allConversations: List<Conversation>,
    live: LiveState,
    blocked: Boolean,
    onBack: () -> Unit,
    onMessage: () -> Unit,
    onVerify: () -> Unit,
    onToggleFavorite: () -> Unit,
    onToggleMuted: () -> Unit,
    onSetDisappearing: (Disappearing) -> Unit,
    onSetBlocked: suspend (Boolean) -> Unit,
    onReport: suspend (String) -> Unit,
    /** Voice or video call; null when calling isn't available. */
    onCall: ((kind: String) -> Unit)? = null,
    onOpenSharedMedia: () -> Unit = {},
    /** Drawn under the action tiles (a friend's story highlights). */
    extra: @Composable () -> Unit = {},
) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    if (conversation == null) {
        LaunchedEffect(Unit) { onBack() }
        return
    }
    val userId = conversation.meta.otherUserId
    val online = userId in live.online
    val favorite = userId in live.favorites
    val muted = userId in live.muted
    var pickingTimer by remember { mutableStateOf(false) }
    var confirmBlock by remember { mutableStateOf(false) }
    var reporting by remember { mutableStateOf(false) }
    var reason by remember { mutableStateOf("") }
    var notice by remember { mutableStateOf<String?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    val files = conversation.messages.count { it.kind == MessageKind.File }
    val common = allConversations.count { c -> c.isGroup && c.meta.members.any { it.userId == userId } }

    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(start = 8.dp, end = 12.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            RoundButton(R.drawable.ui_chevron_left, "Back to contacts", onClick = onBack, container = Color.Transparent, iconSize = 24.dp)
            Spacer(Modifier.weight(1f))
            RoundButton(
                R.drawable.ui_star,
                if (favorite) "Remove from favorites" else "Add to favorites",
                onClick = onToggleFavorite,
                container = if (favorite) colors.toggleOnBg else Color.Transparent,
                tint = if (favorite) colors.toggleOnFg else colors.text,
            )
        }
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Avatar(conversation.avatarUrl, conversation.handle, size = 96.dp, online = online)
                Text(
                    conversation.handle,
                    fontFamily = RedvaultFonts.display,
                    fontSize = 28.sp,
                    fontWeight = FontWeight.Bold,
                    color = colors.text,
                    modifier = Modifier.padding(top = 4.dp),
                )
                Text("@${conversation.handle}" + if (online) " · online" else "", fontFamily = MonoFamily, fontSize = 13.sp, color = if (online) colors.accent else colors.text3)
            }
            Row(Modifier.fillMaxWidth().padding(top = 6.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ActionTile("Message", R.drawable.ui_chat, enabled = !blocked, onClick = onMessage)
                if (onCall != null) {
                    ActionTile("Voice", R.drawable.ui_phone, enabled = !blocked, onClick = { onCall("voice") })
                    ActionTile("Video", R.drawable.ui_video, enabled = !blocked, onClick = { onCall("video") })
                } else {
                    ActionTile("Favorite", R.drawable.ui_star, on = favorite, onClick = onToggleFavorite)
                }
                ActionTile(if (muted) "Muted" else "Mute", if (muted) R.drawable.ui_bell_off else R.drawable.ui_bell, on = muted, onClick = onToggleMuted)
            }
            extra()
            // Safety number: changed since first seen, verified, or not yet.
            val keyChanged = conversation.keyChanged
            Row(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(16.dp))
                    .background(if (keyChanged) colors.warnSoft else if (conversation.verified) Color(0xFF16271F) else colors.surface)
                    .border(
                        1.dp,
                        if (keyChanged) colors.warn.copy(alpha = 0.35f) else if (conversation.verified) Color(0xFF244536) else colors.border,
                        RoundedCornerShape(16.dp),
                    )
                    .clickable(role = Role.Button, onClick = onVerify)
                    .padding(14.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Box(
                    Modifier.size(40.dp).clip(RoundedCornerShape(20.dp)).background(
                        if (keyChanged) colors.warn.copy(alpha = 0.18f) else if (conversation.verified) colors.accent else colors.surface3,
                    ),
                    contentAlignment = Alignment.Center,
                ) {
                    when {
                        keyChanged -> DsIcon(R.drawable.ic_alert, colors.warn, 20.dp)
                        conversation.verified -> DsIcon(R.drawable.ui_shield_check, colors.accentContrast, 20.dp)
                        else -> DsIcon(R.drawable.ui_shield, colors.text2, 20.dp)
                    }
                }
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(
                        if (keyChanged) "Safety number changed" else if (conversation.verified) "Safety number verified" else "Not verified yet",
                        color = if (keyChanged) colors.warn else colors.text,
                        fontSize = 15.sp,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        when {
                            keyChanged -> "Their key changed since you last talked. Compare the new safety number before you trust this chat."
                            conversation.verified -> "Checked on this phone"
                            else -> "Compare in person or over a call you trust"
                        },
                        color = if (keyChanged) colors.text2 else if (conversation.verified) Color(0xFFB5D9C5) else colors.text3,
                        fontSize = 12.5.sp,
                    )
                }
                DsIcon(R.drawable.ui_chevron_right, colors.text3, 18.dp)
            }
            Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface)) {
                SettingRow(R.drawable.ui_timer, "Disappearing messages", conversation.disappearing.label, onClick = { pickingTimer = true })
                Divider()
                SettingRow(
                    if (muted) R.drawable.ui_bell_off else R.drawable.ui_bell,
                    "Notifications",
                    if (muted) "Muted" else "On",
                    onClick = onToggleMuted,
                )
                Divider()
                SettingRow(R.drawable.ui_image, "Shared media and files", files.toString(), onClick = onOpenSharedMedia)
                Divider()
                SettingRow(R.drawable.ui_users, "Groups in common", common.toString(), onClick = null)
            }
            if (muted) {
                Text(
                    "Muted: their calls won't ring this phone. Message alerts never say who they're from, so they can't be silenced for one person.",
                    color = colors.text3,
                    fontSize = 12.5.sp,
                )
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                DangerTile(if (blocked) "Unblock" else "Block", R.drawable.ui_ban, Modifier.weight(1f)) {
                    if (blocked) scope.launch { runCatching { onSetBlocked(false) }.onSuccess { notice = "Unblocked." }.onFailure { error = "Couldn't change that. Try again." } }
                    else confirmBlock = true
                }
                DangerTile("Report", R.drawable.ui_flag, Modifier.weight(1f)) { reporting = !reporting }
            }
            if (reporting) {
                Text("Moderators see your report and this account, never your messages: they're end-to-end encrypted.", color = colors.text3, fontSize = 13.sp)
                DsTextField(reason, { if (it.length <= 1000) reason = it }, "What happened?", singleLine = false, minHeight = 90.dp)
                PillButton(
                    "Send report",
                    {
                        scope.launch {
                            runCatching { onReport(reason.trim()) }
                                .onSuccess { reporting = false; reason = ""; notice = "Report sent. Thank you."; error = null }
                                .onFailure { error = "Couldn't send the report. Try again." }
                        }
                    },
                    Modifier.fillMaxWidth(),
                    enabled = reason.isNotBlank(),
                )
            }
            error?.let { Callout(it, CalloutTone.Error, R.drawable.ui_alert_circle) }
            notice?.let { Callout(it, CalloutTone.Ok, R.drawable.ui_check) }
            Spacer(Modifier.height(16.dp))
        }
    }

    if (pickingTimer) {
        AlertDialog(
            onDismissRequest = { pickingTimer = false },
            containerColor = colors.surface2,
            title = { Text("Disappearing messages", color = colors.text) },
            text = {
                Column {
                    Disappearing.entries.forEach { d ->
                        Row(
                            Modifier.fillMaxWidth().clickable { onSetDisappearing(d); pickingTimer = false }.padding(vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(d.label, color = colors.text, fontSize = 15.sp, modifier = Modifier.weight(1f))
                            if (d == conversation.disappearing) DsIcon(R.drawable.ui_check, colors.accent, 18.dp)
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { pickingTimer = false }) { Text("Close", color = colors.text2) } },
        )
    }
    if (confirmBlock) {
        AlertDialog(
            onDismissRequest = { confirmBlock = false },
            containerColor = colors.surface2,
            title = { Text("Block ${conversation.handle}?", color = colors.text) },
            text = { Text("Neither of you will be able to message or call the other until you unblock them.", color = colors.text2) },
            confirmButton = {
                TextButton(onClick = {
                    confirmBlock = false
                    scope.launch { runCatching { onSetBlocked(true) }.onSuccess { notice = "Blocked." }.onFailure { error = "Couldn't change that. Try again." } }
                }) { Text("Block", color = colors.danger) }
            },
            dismissButton = { TextButton(onClick = { confirmBlock = false }) { Text("Cancel", color = colors.text2) } },
        )
    }
}

@Composable
private fun androidx.compose.foundation.layout.RowScope.ActionTile(label: String, icon: Int, on: Boolean = false, enabled: Boolean = true, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Column(
        Modifier
            .weight(1f)
            .height(64.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(if (on) colors.toggleOnBg else colors.surface2)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .semantics { contentDescription = label },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterVertically),
    ) {
        val ink = when {
            !enabled -> colors.text3
            on -> colors.toggleOnFg
            else -> colors.text
        }
        DsIcon(icon, ink, 20.dp)
        Text(label, color = ink, fontSize = 12.sp, fontWeight = FontWeight.Medium)
    }
}

@Composable
private fun SettingRow(icon: Int, label: String, value: String, onClick: (() -> Unit)?) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 52.dp)
            .then(if (onClick != null) Modifier.clickable(role = Role.Button, onClick = onClick) else Modifier)
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        DsIcon(icon, colors.text2, 18.dp)
        Text(label, color = colors.text, fontSize = 15.sp, modifier = Modifier.weight(1f))
        Text(value, color = colors.text3, fontSize = 14.sp)
        if (onClick != null) DsIcon(R.drawable.ui_chevron_right, colors.text3, 16.dp)
    }
}

@Composable
private fun Divider() = Box(Modifier.fillMaxWidth().height(1.dp).background(RedvaultTheme.colors.border))

@Composable
private fun DangerTile(label: String, icon: Int, modifier: Modifier, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        modifier.height(48.dp).clip(RoundedCornerShape(14.dp)).background(colors.surface).clickable(role = Role.Button, onClick = onClick),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
    ) {
        DsIcon(icon, colors.danger, 18.dp)
        Text(label, color = colors.danger, fontSize = 15.sp, fontWeight = FontWeight.Medium)
    }
}

// --- Your contact code ---

/** Your contact code: a QR code of your username. People add you by it; no phone number or email is involved. */
@Composable
fun ContactCodeScreen(handle: String, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val qr = remember(handle) { qrBitmap(handle) }
    var copied by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(start = 8.dp, top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            RoundButton(R.drawable.ui_chevron_left, "Back", onClick = onBack, container = Color.Transparent, iconSize = 24.dp)
        }
        Column(
            Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Text("Your contact code", fontFamily = RedvaultFonts.display, fontSize = 28.sp, fontWeight = FontWeight.Bold, color = colors.text)
            Text(
                "People add you by your username; no phone number or email is ever involved. Show this code, or send your username.",
                color = colors.text2,
                fontSize = 14.sp,
                textAlign = TextAlign.Center,
            )
            Image(
                qr,
                contentDescription = "QR code for your username, $handle",
                modifier = Modifier.size(240.dp).clip(RoundedCornerShape(16.dp)).background(Color.White).padding(10.dp),
            )
            Text(
                handle,
                fontFamily = MonoFamily,
                fontSize = 17.sp,
                color = colors.text,
                modifier = Modifier.clip(RoundedCornerShape(12.dp)).background(colors.surface2).padding(horizontal = 18.dp, vertical = 10.dp),
            )
            PillButton(
                if (copied) "Copied" else "Copy username",
                { SecretClipboard.copy(context, handle, secret = false); copied = true },
                Modifier.fillMaxWidth(),
                icon = if (copied) R.drawable.ui_check else R.drawable.ui_copy,
            )
        }
    }
}
