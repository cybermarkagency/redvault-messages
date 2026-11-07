package com.redvaultsystems.redvaultmessage.ui.groups

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.InvitePreview
import com.redvaultsystems.redvaultmessage.ui.chats.ChatSession
import com.redvaultsystems.redvaultmessage.ui.components.DsIcon
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.PrimaryButton
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.launch

/**
 * Someone opened a group invite link (a port of the web's screens/JoinGroup.tsx): the group's name and size,
 * and "Join group", or "Ask to join" when an admin approves new members.
 */
@Composable
fun JoinGroupScreen(linkId: String, secret: String, chats: ChatSession, onBack: () -> Unit, onOpenGroup: (conversationId: String) -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var preview by remember(linkId, secret) { mutableStateOf<InvitePreview?>(null) }
    var requested by remember(linkId, secret) { mutableStateOf(false) }
    var error by remember(linkId, secret) { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }

    LaunchedEffect(linkId, secret) {
        try {
            val opened = chats.openInvite(linkId, secret)
            preview = opened
            requested = opened.requested
        } catch (e: Exception) {
            error = describeError(e, "Couldn't open this link. Try again.")
        }
    }

    val shown = preview
    SubScreen(title = shown?.name ?: "Group invite", onBack = onBack) {
        if (shown == null) {
            error?.let { Notice(it) } ?: Text("Opening the link…", color = colors.text3)
            return@SubScreen
        }
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
            GroupAvatar(shown.name, size = 64.dp)
            Text("Group · ${shown.memberCount} ${if (shown.memberCount == 1) "member" else "members"}", color = colors.text2, fontSize = 14.sp)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            DsIcon(R.drawable.ui_lock, colors.accent, 14.dp)
            Text(
                "Messages in this group are end-to-end encrypted. You'll only see messages sent after you join." +
                    if (shown.approval) " An admin of the group has to approve you first." else "",
                color = colors.text2,
                fontSize = 14.sp,
            )
        }
        error?.let { Notice(it) }
        when {
            shown.member -> PrimaryButton(text = "Open group", enabled = true) { onOpenGroup(shown.conversationId) }
            requested -> {
                Text(
                    "Request sent. The group will appear in your chats once an admin approves it.",
                    color = colors.text3,
                    fontSize = 14.sp,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.fillMaxWidth(),
                )
                PillButton("Cancel request", {
                    busy = true
                    error = null
                    scope.launch {
                        try {
                            chats.cancelJoinRequest(shown)
                            requested = false
                        } catch (e: Exception) {
                            error = "Couldn't cancel your request. Try again."
                        } finally {
                            busy = false
                        }
                    }
                }, Modifier.fillMaxWidth(), style = PillStyle.Secondary, enabled = !busy, busy = busy)
            }
            else -> PrimaryButton(
                text = if (busy) "Joining…" else if (shown.approval) "Ask to join" else "Join group",
                enabled = !busy,
                busy = busy,
            ) {
                busy = true
                error = null
                scope.launch {
                    try {
                        val status = chats.joinFromInvite(shown)
                        if (status == "requested") requested = true else onOpenGroup(shown.conversationId)
                    } catch (e: Exception) {
                        error = describeError(e, "Couldn't join the group. Try again.")
                    } finally {
                        busy = false
                    }
                }
            }
        }
    }
}
