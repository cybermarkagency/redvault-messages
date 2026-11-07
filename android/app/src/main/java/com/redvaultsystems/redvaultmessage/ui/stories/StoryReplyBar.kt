package com.redvaultsystems.redvaultmessage.ui.stories

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.model.RichMessage
import com.redvaultsystems.redvaultmessage.ui.components.RoundButton

/**
 * Under someone else's story: a quick reaction or a few words, sent to them as an ordinary end-to-end encrypted chat message that names the
 * story. The viewer's clock stops while the box has focus.
 */
@Composable
internal fun StoryReplyBar(
    handle: String,
    onFocusChange: (Boolean) -> Unit,
    onSend: (text: String, reaction: Boolean) -> Unit,
) {
    var text by remember { mutableStateOf("") }
    Column(Modifier.fillMaxWidth().imePadding(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
            for (emoji in RichMessage.REACTION_EMOJI) {
                Text(
                    emoji,
                    fontSize = 26.sp,
                    modifier = Modifier
                        .clip(RoundedCornerShape(20.dp))
                        .clickable { onSend(emoji, true) }
                        .semantics { contentDescription = "React with $emoji" }
                        .padding(horizontal = 8.dp, vertical = 4.dp),
                )
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            BasicTextField(
                value = text,
                onValueChange = { if (it.length <= 500) text = it },
                singleLine = true,
                textStyle = TextStyle(color = Color.White, fontSize = 15.sp),
                cursorBrush = SolidColor(Color.White),
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(22.dp))
                    .background(StoryStyle.scrim)
                    .onFocusChanged { onFocusChange(it.isFocused) }
                    .padding(horizontal = 16.dp, vertical = 12.dp)
                    .semantics { contentDescription = "Reply to $handle" },
                decorationBox = { inner ->
                    if (text.isEmpty()) Text("Reply to $handle", color = Color(0x99FFFFFF), fontSize = 15.sp)
                    inner()
                },
            )
            RoundButton(
                R.drawable.ui_send,
                "Send reply",
                onClick = {
                    if (text.isNotBlank()) {
                        onSend(text, false)
                        text = ""
                    }
                },
                size = 44.dp,
                container = StoryStyle.scrim,
                tint = Color.White,
                enabled = text.isNotBlank(),
            )
        }
    }
}
