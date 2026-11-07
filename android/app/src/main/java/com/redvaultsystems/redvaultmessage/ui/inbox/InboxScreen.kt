package com.redvaultsystems.redvaultmessage.ui.inbox

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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.redvaultsystems.redvaultmessage.data.api.AnnouncementDto
import com.redvaultsystems.redvaultmessage.data.api.AnnouncementReplyDto
import com.redvaultsystems.redvaultmessage.ui.TimeFormat
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import java.time.Instant

/**
 * Messages and polls from the Redvault team. They are not end-to-end encrypted (the server counts the votes),
 * and the screen says so. Opening it marks everything read.
 */
@Composable
fun InboxScreen(session: InboxSession, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    val state by session.state.collectAsStateWithLifecycle()
    LaunchedEffect(Unit) { session.refreshNow() }
    // Everything on the page counts as read, including what arrives while it is open.
    LaunchedEffect(state.items.size, state.unread) { session.markAllRead() }
    LaunchedEffect(state.items.map { it.id }) {
        state.items.filter { it.allowReplies }.forEach { session.loadReplies(it.id) }
    }

    SubScreen(title = "Inbox", onBack = onBack) {
        Text(
            "Messages and polls from the Redvault team. These are not end-to-end encrypted: the server reads them to count votes and replies. Your replies go to the team only.",
            color = colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        state.error?.let { Notice(it) }
        if (state.loaded && state.items.isEmpty()) Text("Nothing from the team yet.", color = colors.text3)
        state.items.forEach { item ->
            AnnouncementCard(
                item = item,
                replies = state.replies[item.id].orEmpty(),
                busy = state.busy,
                onVote = { optionId -> session.vote(item.id, optionId) },
                onReply = { body, done -> session.reply(item.id, body, done) },
            )
        }
    }
}

@Composable
private fun AnnouncementCard(
    item: AnnouncementDto,
    replies: List<AnnouncementReplyDto>,
    busy: Boolean,
    onVote: (String) -> Unit,
    onReply: (body: String, onSent: () -> Unit) -> Unit,
) {
    val colors = RedvaultTheme.colors
    var draft by remember(item.id) { mutableStateOf("") }
    Column(
        Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(14.dp)).padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(
            runCatching { TimeFormat.longDate(Instant.parse(item.createdAt).toEpochMilli()) }.getOrDefault(""),
            color = colors.text3,
            style = MaterialTheme.typography.labelSmall,
        )
        // Plain text: the team's words are shown as written, with no formatting or link previews.
        Text(item.body, color = colors.text)
        if (item.kind == "poll") {
            val voted = item.myOptionId
            item.options.forEach { option ->
                val mine = voted == option.id
                PillButton(
                    if (mine) "✓ ${option.label}" else option.label,
                    onClick = { onVote(option.id) },
                    modifier = Modifier.fillMaxWidth(),
                    // Votes are final: once one is cast the options stop taking taps.
                    style = if (mine) PillStyle.Primary else PillStyle.Secondary,
                    enabled = voted == null && !busy,
                    height = 44.dp,
                )
            }
            Text(
                if (voted == null) "One vote each, and it is final." else "Your vote is in.",
                color = colors.text3,
                style = MaterialTheme.typography.labelSmall,
            )
        }
        if (item.allowReplies) {
            replies.forEach { reply ->
                Column(Modifier.fillMaxWidth().background(colors.bg, RoundedCornerShape(10.dp)).padding(10.dp)) {
                    Text("You", color = colors.text3, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.labelSmall)
                    Text(reply.body, color = colors.text)
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                DsTextField(draft, { draft = it.take(2000) }, "Reply to the team", modifier = Modifier.weight(1f), singleLine = false)
                PillButton("Send", onClick = { onReply(draft) { draft = "" } }, enabled = draft.isNotBlank() && !busy, height = 50.dp)
            }
        }
    }
}
