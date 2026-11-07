package com.redvaultsystems.redvaultmessage.ui.groups

import android.content.Intent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.InviteLinkView
import com.redvaultsystems.redvaultmessage.data.JoinRequestView
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.ui.chats.ChatSession
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.PrimaryButton
import com.redvaultsystems.redvaultmessage.ui.components.RvSwitch
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.ui.components.SectionLabel
import com.redvaultsystems.redvaultmessage.ui.components.SettingsRow
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Group info > Invite link, a port of the web's screens/GroupInviteLink.tsx:
 * every member can copy or share the group's link; the owner and admins turn
 * it on or off, reset it, choose whether they approve each join, and answer
 * requests to join.
 */
@Composable
fun GroupInviteSection(conversation: Conversation, chats: ChatSession, canManage: Boolean) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val groupId = conversation.id
    var loaded by remember(groupId) { mutableStateOf(false) }
    var link by remember(groupId) { mutableStateOf<InviteLinkView?>(null) }
    var requests by remember(groupId) { mutableStateOf<List<JoinRequestView>>(emptyList()) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var copied by remember { mutableStateOf(false) }
    var confirm by remember { mutableStateOf<Pair<String, suspend () -> Unit>?>(null) }

    suspend fun loadRequests() {
        if (canManage) requests = chats.joinRequests(groupId)
    }

    LaunchedEffect(groupId, canManage) {
        try {
            link = chats.inviteLink(groupId)
            loadRequests()
        } catch (e: Exception) {
            link = null
        }
        loaded = true
    }

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

    if (!loaded || (link == null && !canManage)) return

    SectionLabel("Invite link")
    error?.let { Notice(it) }
    val current = link
    if (current == null) {
        Text(
            "Anyone with the link can join, or ask to join if you approve new members. The group stays end-to-end encrypted: " +
                "the link's secret never reaches the server.",
            color = colors.text2,
            fontSize = 14.sp,
        )
        PrimaryButton(text = "Create invite link", enabled = !busy, busy = busy) {
            run("Couldn't create the link.") { link = chats.newInviteLink(groupId, approval = true) }
        }
    } else {
        val url = current.url
        if (url != null) {
            Column(
                Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(16.dp)).padding(14.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                SelectionContainer {
                    Text(url, color = colors.text, fontFamily = RedvaultFonts.mono, fontSize = 13.sp)
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    PillButton(
                        if (copied) "Copied" else "Copy",
                        {
                            SecretClipboard.copy(context, url, secret = false)
                            copied = true
                            scope.launch {
                                delay(2000)
                                copied = false
                            }
                        },
                        style = PillStyle.Secondary,
                        height = 36.dp,
                        icon = R.drawable.ui_copy,
                    )
                    PillButton("Share", { shareInvite(context, url) }, style = PillStyle.Secondary, height = 36.dp, icon = R.drawable.ui_share)
                }
            }
        } else {
            Text("This group's link couldn't be opened on this device.", color = colors.text3, fontSize = 14.sp)
        }
        if (canManage) {
            val secret = current.secret
            SettingsRow(
                title = "Approve new members",
                description = if (current.approval) "An admin lets in each person who opens the link." else "Anyone who opens the link joins straight away.",
                trailing = {
                    RvSwitch(
                        checked = current.approval,
                        // Turning approval off wraps the group key under the link's key, so it needs the secret.
                        onCheckedChange = if (busy || secret == null) null else { on ->
                            run("Couldn't change that setting.") { link = chats.setInviteApproval(groupId, on, secret) }
                        },
                        description = "Approve new members",
                    )
                },
            )
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                TextButton(enabled = !busy, onClick = {
                    confirm = "Reset the invite link? The current link stops working." to { link = chats.newInviteLink(groupId, current.approval) }
                }) { Text("Reset link", color = colors.accent) }
                TextButton(enabled = !busy, onClick = {
                    confirm = "Turn off the invite link? It stops working, and pending requests are dropped." to {
                        chats.turnOffInviteLink(groupId)
                        link = null
                        requests = emptyList()
                    }
                }) { Text("Turn off link", color = colors.danger) }
            }
        }
    }

    if (canManage && requests.isNotEmpty()) {
        SectionLabel("Requests to join (${requests.size})")
        requests.forEach { request ->
            Row(
                Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(12.dp)).padding(horizontal = 12.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Avatar(avatarUrl = request.avatarUrl, seed = request.handle, size = 32.dp)
                Text(request.handle, fontFamily = FontFamily.Monospace, color = colors.text, modifier = Modifier.weight(1f))
                TextButton(enabled = !busy, onClick = {
                    run("Couldn't let them in.") {
                        chats.approveJoinRequest(groupId, request.userId)
                        loadRequests()
                    }
                }) { Text("Approve", color = colors.accent) }
                TextButton(enabled = !busy, onClick = {
                    run("Couldn't decline the request.") {
                        chats.declineJoinRequest(groupId, request.userId)
                        loadRequests()
                    }
                }) { Text("Decline", color = colors.text2) }
            }
        }
    }

    confirm?.let { (question, action) ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            containerColor = colors.surface,
            text = { Text(question, color = colors.text) },
            confirmButton = {
                TextButton(onClick = {
                    confirm = null
                    run("That didn't work. Try again.", action)
                }) { Text("Yes", color = colors.danger) }
            },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("Cancel", color = colors.text2) } },
        )
    }
}

private fun shareInvite(context: android.content.Context, url: String) {
    val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, url)
    runCatching { context.startActivity(Intent.createChooser(send, "Share invite link")) }
}
