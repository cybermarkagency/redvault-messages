package com.redvaultsystems.redvaultmessage.ui.chats

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.data.model.TextFormat
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/**
 * Message requests: chats started by people who aren't your friends (the desktop's MessageRequestsDialog).
 * Reading one sends nothing back, so they don't learn you've seen it unless you accept; ignoring hides it
 * without telling them; blocking also stops them messaging or sending friend requests.
 */
@Composable
fun MessageRequestsScreen(
    requests: List<Conversation>,
    /** Accepts (the chat joins your list and opens) or ignores a request; [onDone] says whether it worked. */
    onRespond: (conversationId: String, accept: Boolean, onDone: (Boolean) -> Unit) -> Unit,
    onBlock: suspend (userId: String) -> Unit,
    onOpenChat: (conversationId: String) -> Unit,
    onBack: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    var busyId by remember { mutableStateOf<String?>(null) }
    var error by remember { mutableStateOf<String?>(null) }

    SubScreen(title = "Message requests", onBack = onBack) {
        Text(
            "These people aren't your friends. Reading a request sends nothing back: they don't learn you've seen it unless you accept. Ignoring hides it without telling them.",
            color = colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        error?.let { Notice(it) }
        if (requests.isEmpty()) Text("No message requests.", color = colors.text3)
        requests.forEach { request ->
            val theirs = request.messages.filter { !it.fromMe && it.notice == null }
            Column(
                Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(14.dp)).padding(14.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Avatar(avatarUrl = request.avatarUrl, seed = request.handle, size = 44.dp)
                    Text(request.handle, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.SemiBold, color = colors.text, modifier = Modifier.weight(1f))
                }
                // Their messages so far as plain text: never opened as files, never given a preview of links.
                if (theirs.isEmpty()) {
                    Text("No messages yet.", color = colors.text3)
                } else {
                    theirs.takeLast(5).forEach { message ->
                        val text = if (message.kind == MessageKind.File) "📎 ${message.text}" else TextFormat.toPlain(message.text)
                        Text(text, color = colors.text, maxLines = 6, overflow = TextOverflow.Ellipsis)
                    }
                    if (theirs.size > 5) Text("+ ${theirs.size - 5} earlier", color = colors.text3, style = MaterialTheme.typography.labelSmall)
                }
                val busy = busyId == request.id
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                    PillButton(
                        "Accept",
                        onClick = {
                            busyId = request.id
                            error = null
                            onRespond(request.id, true) { ok ->
                                busyId = null
                                if (ok) onOpenChat(request.id) else error = "Couldn't accept it. Try again."
                            }
                        },
                        modifier = Modifier.weight(1f),
                        enabled = busyId == null,
                        busy = busy,
                        height = 44.dp,
                    )
                    PillButton(
                        "Ignore",
                        onClick = {
                            busyId = request.id
                            error = null
                            onRespond(request.id, false) { ok ->
                                busyId = null
                                if (!ok) error = "Couldn't ignore it. Try again."
                            }
                        },
                        modifier = Modifier.weight(1f),
                        style = PillStyle.Secondary,
                        enabled = busyId == null,
                        height = 44.dp,
                    )
                }
                BlockButton(enabled = busyId == null) {
                    busyId = request.id
                    error = null
                    try {
                        onBlock(request.meta.otherUserId)
                    } catch (e: kotlinx.coroutines.CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        error = describeError(e, "Couldn't block them. Try again.")
                    }
                    onRespond(request.id, false) { ok ->
                        busyId = null
                        if (!ok) error = "Couldn't ignore it. Try again."
                    }
                }
            }
        }
    }
}

/** Ignore the request and block the person; asks once so a stray tap doesn't. */
@Composable
private fun BlockButton(enabled: Boolean, onConfirmed: suspend () -> Unit) {
    var confirming by remember { mutableStateOf(false) }
    var pending by remember { mutableStateOf(false) }
    androidx.compose.runtime.LaunchedEffect(pending) {
        if (pending) {
            try {
                onConfirmed()
            } finally {
                pending = false
            }
        }
    }
    if (confirming) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            PillButton("Block them", onClick = { confirming = false; pending = true }, modifier = Modifier.weight(1f), style = PillStyle.Danger, enabled = enabled, height = 44.dp)
            PillButton("Cancel", onClick = { confirming = false }, modifier = Modifier.weight(1f), style = PillStyle.Quiet, height = 44.dp)
        }
    } else {
        PillButton("Block", onClick = { confirming = true }, modifier = Modifier.fillMaxWidth(), style = PillStyle.Quiet, enabled = enabled, height = 44.dp)
    }
}
