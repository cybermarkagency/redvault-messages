package com.redvaultsystems.redvaultmessage.ui.chats

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.files.FileTile
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private enum class MediaFilter(val label: String) { All("All"), Media("Photos & videos"), Files("Files"), Voice("Voice") }

/**
 * Everything shared in one chat, newest first: the same encrypted files
 * as in the thread, downloaded and decrypted on this phone only when you
 * open one. Disappearing and deleted ones are gone from here too.
 */
@Composable
fun SharedMediaScreen(conversation: Conversation?, onBack: () -> Unit, onDownload: DownloadFile) {
    val colors = RedvaultTheme.colors
    var filterName by rememberSaveable { mutableStateOf(MediaFilter.All.name) }
    val filter = MediaFilter.valueOf(filterName)
    SubScreen(title = "Shared media and files", onBack = onBack) {
        if (conversation == null) {
            Text("This chat isn't available.", color = colors.text3, fontSize = 14.sp)
            return@SubScreen
        }
        val files = conversation.messages.filter { it.kind == MessageKind.File && it.file != null }.sortedByDescending { it.createdAtMs }
        Text("In your chat with ${conversation.handle}. Files are encrypted end to end and decrypted only on this phone.", color = colors.text2, fontSize = 14.sp)
        ChipRow(MediaFilter.entries, filter, { it.label }, { filterName = it.name }, edge = 0.dp)
        val shown = files.filter { matches(it, filter) }
        if (shown.isEmpty()) {
            Text(
                if (files.isEmpty()) "Photos, videos and files you share here show up in this list." else "Nothing of this kind yet.",
                color = colors.text3,
                fontSize = 14.sp,
                modifier = Modifier.padding(vertical = 24.dp),
            )
        }
        shown.forEach { message -> SharedRow(message, conversation, onDownload) }
    }
}

private fun matches(message: Message, filter: MediaFilter): Boolean {
    val mime = message.file?.mimeType.orEmpty()
    return when (filter) {
        MediaFilter.All -> true
        MediaFilter.Media -> mime.startsWith("image/") || mime.startsWith("video/")
        MediaFilter.Voice -> mime.startsWith("audio/")
        MediaFilter.Files -> !mime.startsWith("image/") && !mime.startsWith("video/") && !mime.startsWith("audio/")
    }
}

@Composable
private fun SharedRow(message: Message, conversation: Conversation, onDownload: DownloadFile) {
    val colors = RedvaultTheme.colors
    val descriptor = message.file ?: return
    val from = when {
        message.fromMe -> "You"
        conversation.isGroup -> conversation.meta.members.firstOrNull { it.userId == message.senderId }?.handle ?: "Someone"
        else -> conversation.handle
    }
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(colors.surface).padding(14.dp),
        verticalAlignment = Alignment.Top,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        FileTile(descriptor.filename, folder = false, lockedNow = false, size = 40.dp)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            FileBubbleContent(message, colors.text, colors.text3, onDownload, showIcon = false)
            Text("$from · ${dayText(message.createdAtMs)}", color = colors.text3, fontSize = 12.sp)
        }
    }
}

private fun dayText(ms: Long): String = SimpleDateFormat("MMM d, HH:mm", Locale.getDefault()).format(Date(ms))
