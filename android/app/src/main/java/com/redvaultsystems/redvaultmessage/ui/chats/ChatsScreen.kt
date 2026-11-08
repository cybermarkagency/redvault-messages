// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.crypto.GroupInviteLink
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.data.model.Disappearing
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.data.model.NoticeKind
import com.redvaultsystems.redvaultmessage.data.model.TextFormat
import com.redvaultsystems.redvaultmessage.ui.TimeFormat
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton
import com.redvaultsystems.redvaultmessage.ui.components.SearchBox
import com.redvaultsystems.redvaultmessage.ui.components.SecurityLine
import com.redvaultsystems.redvaultmessage.ui.components.TabHeader
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.groups.GroupAvatar
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/** The Chats tab: title, search, filter chips and the conversation list. */
@Composable
fun ChatsScreen(
    state: ChatsState,
    selfHandle: String,
    selfAvatarUrl: String?,
    incomingRequests: Int,
    onOpen: (conversationId: String) -> Unit,
    onAddFriend: () -> Unit,
    onNewGroup: () -> Unit,
    onOpenRequests: () -> Unit,
    onOpenSettings: () -> Unit,
    online: Set<String> = emptySet(),
    /** A pasted group invite link: open its join screen. */
    onJoinGroupLink: (linkId: String, secret: String) -> Unit = { _, _ -> },
    /** Open the message requests (chats from non-friends); the banner shows only when there are some. */
    onOpenMessageRequests: () -> Unit = {},
    /** Paid and free communities: discover, your own, your memberships. */
    onOpenCommunities: (() -> Unit)? = null,
    /** The row of story rings; draws nothing unless the server has Stories on. */
    stories: @Composable () -> Unit = {},
    /** Contacts (user ids) marked Favorite on this phone: the Favorites chip. */
    favorites: Set<String> = emptySet(),
    /** Contacts (user ids) and groups (chat ids) muted on this phone. */
    muted: Set<String> = emptySet(),
    /** Chats where someone is typing right now (only while typing indicators are on). */
    typingIn: Set<String> = emptySet(),
    /** Chat id -> unsent text, shown as "Draft:". */
    drafts: Map<String, String> = emptyMap(),
) {
    val colors = RedvaultTheme.colors
    var choosing by remember { mutableStateOf(false) }
    var pastingInvite by remember { mutableStateOf(false) }
    var query by rememberSaveable { mutableStateOf("") }
    var filter by rememberSaveable { mutableStateOf(ChatFilter.All) }
    if (choosing) {
        AlertDialog(
            onDismissRequest = { choosing = false },
            containerColor = colors.surface,
            title = { Text("New chat", color = colors.text) },
            text = {
                Column {
                    TextButton(onClick = { choosing = false; onAddFriend() }) { Text("Add a friend", color = colors.accent) }
                    TextButton(onClick = { choosing = false; onNewGroup() }) { Text("New group", color = colors.accent) }
                    TextButton(onClick = { choosing = false; pastingInvite = true }) { Text("Join a group with a link", color = colors.accent) }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { choosing = false }) { Text("Cancel", color = colors.text2) } },
        )
    }

    if (pastingInvite) JoinGroupLinkDialog(onDismiss = { pastingInvite = false }) { linkId, secret ->
        pastingInvite = false
        onJoinGroupLink(linkId, secret)
    }

    val unreadCount = state.conversations.count { it.unread > 0 }
    val shown = state.conversations.filter { c -> ChatList.matches(c, filter, query, favorites) }

    Column(Modifier.fillMaxSize().background(colors.bg)) {
        TabHeader("Chats") {
            onOpenCommunities?.let { RoundButton(R.drawable.ui_globe, "Communities", onClick = it) }
            RoundButton(R.drawable.ui_plus, "New chat", onClick = { choosing = true })
            Box(
                Modifier.clip(CircleShape).clickable(role = Role.Button, onClick = onOpenSettings).semantics { contentDescription = "Your profile and settings" },
            ) { Avatar(avatarUrl = selfAvatarUrl, seed = selfHandle, size = 44.dp) }
        }
        SecurityLine("End-to-end encrypted · keys stay on this device", R.drawable.ui_lock)
        stories()
        SearchBox(query, { query = it }, "Search chats and people", R.drawable.ui_search, Modifier.padding(start = 20.dp, end = 20.dp, top = 14.dp, bottom = 12.dp))
        ChipRow(
            options = ChatFilter.entries.toList(),
            selected = filter,
            label = { if (it == ChatFilter.Unread && unreadCount > 0) "Unread · $unreadCount" else it.name },
            onSelect = { filter = it },
        )
        state.error?.let { ErrorBanner(it) }
        if (incomingRequests > 0) RequestsBanner(incomingRequests, onOpenRequests)
        if (state.requests.isNotEmpty()) MessageRequestsBanner(state.requests.size, onOpenMessageRequests)
        when {
            !state.loaded -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(color = colors.accent)
            }
            state.conversations.isEmpty() -> EmptyChats(onAddFriend)
            shown.isEmpty() -> Text(
                if (query.isNotBlank()) "Nothing matches “${query.trim()}”." else "No chats here.",
                color = colors.text3,
                modifier = Modifier.fillMaxWidth().padding(32.dp),
                textAlign = TextAlign.Center,
            )
            else -> LazyColumn(Modifier.fillMaxSize().padding(top = 8.dp)) {
                items(shown, key = { it.id }) { conversation ->
                    ConversationRow(
                        conversation,
                        online = !conversation.isGroup && conversation.meta.otherUserId in online,
                        muted = ChatList.isMuted(conversation, muted),
                        typing = conversation.id in typingIn,
                        draft = drafts[conversation.id].orEmpty(),
                    ) { onOpen(conversation.id) }
                }
            }
        }
    }
}

@Composable
private fun ConversationRow(conversation: Conversation, online: Boolean, muted: Boolean, typing: Boolean, draft: String, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    val last = conversation.messages.lastOrNull()
    // With a disappearing timer on, the list never shows message content.
    val preview = when {
        conversation.disappearing != Disappearing.Off -> if (last != null) "New message" else "No messages yet"
        last == null -> "No messages yet"
        last.kind == MessageKind.File && last.file?.mimeType?.startsWith("audio/") == true -> "Voice message"
        last.kind == MessageKind.File -> "Encrypted file"
        last.notice != null -> {
            val what = if (last.notice.kind == NoticeKind.Pin) "pinned “${TextFormat.toPlain(last.text)}”" else "unpinned a message"
            if (last.fromMe) "You $what" else what.replaceFirstChar { it.uppercaseChar() }
        }
        else -> {
            // Formatting markers dropped and spoilers hidden, like the web.
            val text = if (last.poll != null) "📊 ${last.text}" else TextFormat.toPlain(last.text)
            if (last.fromMe && text.isNotEmpty()) "You: $text" else text.ifEmpty { "No messages yet" }
        }
    }
    val previewIcon = when {
        last?.kind == MessageKind.File && last.file?.mimeType?.startsWith("audio/") == true -> R.drawable.ui_timer
        last?.kind == MessageKind.File -> R.drawable.ui_paperclip
        else -> null
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(if (conversation.unread > 0) colors.selected.copy(alpha = 0.55f) else colors.bg)
            .clickable(onClick = onClick)
            .heightIn(min = 72.dp)
            .padding(horizontal = 20.dp, vertical = 11.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        if (conversation.isGroup) GroupAvatar(conversation.handle, size = 50.dp)
        else Avatar(avatarUrl = conversation.avatarUrl, seed = conversation.handle, size = 50.dp, online = online)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    conversation.handle,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 16.sp,
                    color = colors.text,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f, fill = false),
                )
                if (!conversation.isGroup && conversation.verified) {
                    DsIcon(R.drawable.ui_shield_check, colors.accent, 15.dp, description = "Verified")
                }
                if (muted) DsIcon(R.drawable.ui_bell_off, colors.text3, 14.dp, description = "Muted")
                Box(Modifier.weight(1f))
                if (last != null) {
                    Text(TimeFormat.listLabel(last.createdAtMs), fontSize = 12.sp, color = if (conversation.unread > 0) colors.accent else colors.text3)
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                // Like the desktop: someone typing, else your unsent draft, else the last message.
                val showDraft = !typing && draft.isNotBlank()
                if (showDraft) {
                    Text("Draft:", color = colors.warn, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                } else if (!typing && previewIcon != null) {
                    DsIcon(previewIcon, colors.text3, 14.dp)
                }
                Text(
                    when {
                        typing -> "typing…"
                        // With a disappearing timer on, the list never shows what's written there.
                        showDraft && conversation.disappearing != Disappearing.Off -> "Not sent yet"
                        showDraft -> draft.replace('\n', ' ')
                        else -> preview
                    },
                    color = if (typing) colors.accent else colors.text2,
                    fontSize = 14.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                if (conversation.unread > 0) {
                    Box(
                        modifier = Modifier.size(22.dp).background(if (muted) colors.surface3 else colors.accent, CircleShape),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            if (conversation.unread > 99) "99+" else conversation.unread.toString(),
                            color = if (muted) colors.text2 else colors.accentContrast,
                            fontSize = 11.sp,
                            fontWeight = FontWeight.Bold,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun RequestsBanner(count: Int, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        modifier = Modifier
            .padding(horizontal = 20.dp, vertical = 8.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(colors.accentSoft)
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        DsIcon(R.drawable.ui_user_plus, colors.accent, 18.dp)
        Text(
            if (count == 1) "1 friend request waiting" else "$count friend requests waiting",
            color = colors.accent,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.weight(1f),
        )
        Text("Review", color = colors.accent)
    }
}

/** Chats from people who aren't friends: nothing is sent back until one is accepted. */
@Composable
private fun MessageRequestsBanner(count: Int, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        modifier = Modifier
            .padding(horizontal = 20.dp, vertical = 8.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(colors.surface)
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        DsIcon(R.drawable.ui_lock, colors.text2, 18.dp)
        Text(
            if (count == 1) "1 message request" else "$count message requests",
            color = colors.text,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.weight(1f),
        )
        Text("Review", color = colors.accent)
    }
}

/** "Join a group with a link": paste an invite link someone shared (the web's NewChooser). */
@Composable
private fun JoinGroupLinkDialog(onDismiss: () -> Unit, onOpen: (linkId: String, secret: String) -> Unit) {
    val colors = RedvaultTheme.colors
    var text by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = colors.surface,
        title = { Text("Join a group with a link", color = colors.text) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(
                    "Paste an invite link someone shared with you, or the whole message it came in. Its part after # stays on this phone " +
                        "and is never sent to the server.",
                    color = colors.text2,
                    fontSize = 14.sp,
                )
                DsTextField(text, { text = it; error = null }, "Paste the invite link", mono = true, singleLine = false)
                error?.let { Text(it, color = colors.danger, fontSize = 13.sp) }
            }
        },
        confirmButton = {
            TextButton(enabled = text.isNotBlank(), onClick = {
                val invite = GroupInviteLink.parse(text)
                if (invite == null) error = "That doesn't look like a group invite link. Paste the whole link, including the part after #."
                else onOpen(invite.linkId, invite.secret)
            }) { Text("Open", color = colors.accent) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel", color = colors.text2) } },
    )
}

@Composable
private fun EmptyChats(onAddFriend: () -> Unit) {
    val colors = RedvaultTheme.colors
    Column(
        modifier = Modifier.fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp, Alignment.CenterVertically),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("No chats yet", fontFamily = RedvaultFonts.display, fontWeight = FontWeight.Bold, fontSize = 22.sp, color = colors.text)
        Text(
            "Add a friend by their handle. Your chat appears here once they accept.",
            textAlign = TextAlign.Center,
            color = colors.text2,
            fontSize = 14.sp,
        )
        PillButton("Add a friend", onAddFriend, style = PillStyle.Primary, height = 46.dp, icon = R.drawable.ui_user_plus)
    }
}

@Composable
internal fun ErrorBanner(text: String) {
    val colors = RedvaultTheme.colors
    Text(
        text,
        color = colors.warn,
        fontSize = 13.sp,
        modifier = Modifier.padding(horizontal = 20.dp, vertical = 6.dp).fillMaxWidth().background(colors.warnSoft, RoundedCornerShape(12.dp)).padding(horizontal = 14.dp, vertical = 10.dp),
    )
}
