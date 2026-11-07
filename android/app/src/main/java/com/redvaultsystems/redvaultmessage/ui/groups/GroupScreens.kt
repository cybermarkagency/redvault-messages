package com.redvaultsystems.redvaultmessage.ui.groups

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.redvaultsystems.redvaultmessage.data.api.FriendDto
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.data.model.GroupMemberView
import com.redvaultsystems.redvaultmessage.ui.chats.ChatSession
import com.redvaultsystems.redvaultmessage.ui.components.InitialsAvatar
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.KeyGlyph
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.PrimaryButton
import com.redvaultsystems.redvaultmessage.ui.components.SectionLabel
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.launch

/** Port of ui/src/components/GroupGlyph.tsx: two members' glyphs, overlapping. */
@Composable
fun GroupAvatar(name: String, size: Dp = 40.dp) {
    // The design's group avatar: the group's initials on a rounded square.
    InitialsAvatar(name, size, square = true)
}

/** Pick a name and some friends. The group key is sealed for each; the name is encrypted with it. */
@Composable
fun NewGroupScreen(chats: ChatSession, onBack: () -> Unit, onCreated: (conversationId: String) -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var name by rememberSaveable { mutableStateOf("") }
    var friends by remember { mutableStateOf<List<FriendDto>?>(null) }
    val selected = remember { mutableStateListOf<String>() }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) {
        friends = try {
            chats.friends()
        } catch (e: Exception) {
            error = describeError(e, "Couldn't load your friends.")
            emptyList()
        }
    }

    SubScreen(title = "New group", onBack = onBack) {
        OutlinedTextField(
            value = name,
            onValueChange = { if (it.length <= 100) name = it },
            label = { Text("Group name") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        SectionLabel("Members (${selected.size} selected)")
        val list = friends
        when {
            list == null -> Text("Loading your friends…", color = colors.text3)
            list.isEmpty() -> Text("Add a friend first: groups can only include your friends.", color = colors.text3)
            else -> list.forEach { friend ->
                val isSelected = friend.userId in selected
                Row(
                    Modifier
                        .fillMaxWidth()
                        .background(colors.surface, RoundedCornerShape(12.dp))
                        .clickable { if (isSelected) selected.remove(friend.userId) else selected.add(friend.userId) }
                        .padding(horizontal = 12.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Avatar(avatarUrl = friend.avatarUrl, seed = friend.username, size = 36.dp)
                    Text(friend.username, fontFamily = FontFamily.Monospace, color = colors.text, modifier = Modifier.weight(1f))
                    Checkbox(checked = isSelected, onCheckedChange = null)
                }
            }
        }
        error?.let { Notice(it) }
        PrimaryButton(text = if (busy) "Creating…" else "Create group", enabled = name.isNotBlank() && selected.isNotEmpty(), busy = busy) {
            val members = friends.orEmpty().filter { it.userId in selected }
            busy = true
            error = null
            scope.launch {
                try {
                    onCreated(chats.createGroup(name, members))
                } catch (e: Exception) {
                    error = describeError(e, "Couldn't create the group. Try again.")
                } finally {
                    busy = false
                }
            }
        }
    }
}

/** Members and roles; owners/admins can rename, add friends and remove members; anyone can leave. */
@Composable
fun GroupInfoScreen(
    conversation: Conversation,
    chats: ChatSession,
    onBack: () -> Unit,
    onLeft: () -> Unit,
    onOpenSharedMedia: () -> Unit = {},
) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    val meta = conversation.meta
    val canManage = meta.myRole == "owner" || meta.myRole == "admin"
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var renaming by remember { mutableStateOf<String?>(null) }
    var addable by remember { mutableStateOf<List<FriendDto>?>(null) }
    var confirm by remember { mutableStateOf<Pair<String, suspend () -> Unit>?>(null) }

    fun run(fallback: String, action: suspend () -> Unit) {
        busy = true
        error = null
        scope.launch {
            try {
                action()
            } catch (e: Exception) {
                error = describeError(e, fallback)
            } finally {
                busy = false
            }
        }
    }

    fun canRemove(m: GroupMemberView) =
        !m.isSelf && m.role != "owner" && (meta.myRole == "owner" || (meta.myRole == "admin" && m.role == "member"))

    SubScreen(title = "Group info", onBack = onBack) {
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
            GroupAvatar(meta.handle, size = 64.dp)
            val draft = renaming
            if (draft == null) {
                Text(conversation.handle, style = MaterialTheme.typography.titleLarge, color = colors.text)
                if (canManage) TextButton(onClick = { renaming = conversation.handle }) { Text("Rename") }
            } else {
                OutlinedTextField(value = draft, onValueChange = { if (it.length <= 100) renaming = it }, singleLine = true, label = { Text("Group name") })
                TextButton(enabled = draft.isNotBlank() && !busy, onClick = {
                    run("Couldn't rename the group.") {
                        chats.renameGroup(conversation.id, draft)
                        renaming = null
                    }
                }) { Text("Save name") }
            }
        }
        com.redvaultsystems.redvaultmessage.ui.components.SettingsRow(
            title = "Shared media and files",
            description = conversation.messages.count { it.kind == com.redvaultsystems.redvaultmessage.data.model.MessageKind.File }.let { if (it == 0) "Nothing shared yet" else "$it shared in this group" },
            onClick = onOpenSharedMedia,
            icon = com.redvaultsystems.redvaultmessage.R.drawable.ui_image,
        )
        error?.let { Notice(it) }

        SectionLabel("Members (${meta.members.size})")
        meta.members.forEach { m ->
            Row(
                Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(12.dp)).padding(horizontal = 12.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Avatar(avatarUrl = m.avatarUrl, seed = m.handle, size = 36.dp)
                Column(Modifier.weight(1f)) {
                    Text(if (m.isSelf) "${m.handle} (you)" else m.handle, fontFamily = FontFamily.Monospace, color = colors.text)
                    if (m.role != "member") Text(m.role.replaceFirstChar { it.uppercase() }, color = colors.text3, style = MaterialTheme.typography.labelSmall)
                }
                if (meta.myRole == "owner" && !m.isSelf && m.role != "owner") {
                    TextButton(enabled = !busy, onClick = {
                        run("Couldn't change their role.") { chats.setGroupRole(conversation.id, m.userId, if (m.role == "admin") "member" else "admin") }
                    }) { Text(if (m.role == "admin") "Unadmin" else "Admin") }
                }
                if (canRemove(m)) {
                    TextButton(enabled = !busy, onClick = {
                        confirm = "Remove ${m.handle} from the group?" to { chats.removeFromGroup(conversation.id, m.userId) }
                    }) { Text("Remove", color = colors.danger) }
                }
            }
        }

        if (canManage) {
            val candidates = addable
            if (candidates == null) {
                TextButton(enabled = !busy, onClick = {
                    run("Couldn't load your friends.") {
                        val inGroup = meta.members.map { it.userId }.toSet()
                        addable = chats.friends().filter { it.userId !in inGroup }
                    }
                }) { Text("Add friends") }
            } else {
                SectionLabel("Add friends")
                if (candidates.isEmpty()) Text("All your friends are already in this group.", color = colors.text3)
                candidates.forEach { friend ->
                    Row(
                        Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(12.dp)).padding(horizontal = 12.dp, vertical = 6.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        Avatar(avatarUrl = friend.avatarUrl, seed = friend.username, size = 36.dp)
                        Text(friend.username, fontFamily = FontFamily.Monospace, color = colors.text, modifier = Modifier.weight(1f))
                        TextButton(enabled = !busy, onClick = {
                            run("Couldn't add them.") {
                                chats.addToGroup(conversation.id, friend)
                                addable = addable?.filterNot { it.userId == friend.userId }
                            }
                        }) { Text("Add") }
                    }
                }
                Text("New members only see messages sent after they join.", color = colors.text3, style = MaterialTheme.typography.bodySmall)
            }
        }

        GroupInviteSection(conversation, chats, canManage)

        TextButton(
            enabled = !busy,
            onClick = {
                confirm = "Leave this group? You'll stop receiving its messages." to {
                    chats.leaveGroup(conversation.id)
                    onLeft()
                }
            },
            modifier = Modifier.fillMaxWidth(),
        ) { Text("Leave group", color = colors.danger, fontWeight = FontWeight.SemiBold) }
    }

    confirm?.let { (question, action) ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            text = { Text(question) },
            confirmButton = {
                TextButton(onClick = {
                    confirm = null
                    run("That didn't work. Try again.", action)
                }) { Text("Yes", color = colors.danger) }
            },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("Cancel") } },
        )
    }
}
