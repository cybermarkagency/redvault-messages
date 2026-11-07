package com.redvaultsystems.redvaultmessage.ui.chats

import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.SearchBox
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.groups.GroupAvatar
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/**
 * Files and text shared from another app (Android's share sheet). Held in memory only; they wait in the
 * chosen chat's message box, and nothing is sent until you press Send there.
 */
class SharedItems(val uris: List<Uri>, val text: String)

/** "Share to": the chat the shared files or text go to, with a search box. */
@Composable
fun ShareToScreen(items: SharedItems?, conversations: List<Conversation>, onPick: (conversationId: String) -> Unit, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    var query by remember { mutableStateOf("") }
    SubScreen(title = "Share to", onBack = onBack) {
        if (items == null) {
            // The app was restarted since: the files have to be shared again.
            Text("Nothing to share any more. Share it again from the other app.", color = colors.text3, fontSize = 14.sp)
            return@SubScreen
        }
        val what = listOfNotNull(
            items.uris.size.takeIf { it > 0 }?.let { if (it == 1) "1 file" else "$it files" },
            "text".takeIf { items.text.isNotBlank() },
        ).joinToString(" and ")
        Text(
            if (items.uris.size + (if (items.text.isNotBlank()) 1 else 0) > 1) {
                "Pick the chat for $what. They wait in the message box there, and are encrypted on this phone only when you send them."
            } else {
                "Pick the chat for $what. It waits in the message box there, and is encrypted on this phone only when you send it."
            },
            color = colors.text2,
            fontSize = 14.sp,
        )
        SearchBox(query, { query = it }, "Search chats", R.drawable.ui_search)
        val shown = conversations.filter { query.isBlank() || it.handle.contains(query.trim(), ignoreCase = true) }
        if (shown.isEmpty()) {
            Text(if (conversations.isEmpty()) "You don't have a chat to share to yet." else "No chats match.", color = colors.text3, fontSize = 14.sp)
        }
        shown.forEach { conversation ->
            Row(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(14.dp))
                    .background(colors.surface)
                    .clickable { onPick(conversation.id) }
                    .padding(horizontal = 14.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                if (conversation.isGroup) GroupAvatar(conversation.handle, size = 40.dp)
                else Avatar(avatarUrl = conversation.avatarUrl, seed = conversation.handle, size = 40.dp)
                Text(
                    conversation.handle,
                    color = colors.text,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}
