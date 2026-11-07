package com.redvaultsystems.redvaultmessage.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.redvaultsystems.redvaultmessage.data.api.BlockedUserDto
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.launch

/** Everyone this user has blocked, with Unblock. */
@Composable
fun BlockedUsersScreen(blocked: List<BlockedUserDto>, onUnblock: suspend (String) -> Unit, onBack: () -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var busyId by remember { mutableStateOf<String?>(null) }
    var error by remember { mutableStateOf<String?>(null) }

    SubScreen(title = "Blocked users", onBack = onBack) {
        Text(
            "Blocked people can't message you or send you friend requests, and you can't message them. They aren't told.",
            color = colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        error?.let { Notice(it) }
        if (blocked.isEmpty()) Text("You haven't blocked anyone.", color = colors.text3)
        blocked.forEach { user ->
            Row(
                Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(12.dp)).padding(12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Avatar(avatarUrl = user.avatarUrl, seed = user.username, size = 40.dp)
                Text(user.username, fontFamily = FontFamily.Monospace, color = colors.text, modifier = Modifier.weight(1f))
                TextButton(enabled = busyId == null, onClick = {
                    busyId = user.userId
                    error = null
                    scope.launch {
                        try {
                            onUnblock(user.userId)
                        } catch (e: Exception) {
                            error = describeError(e, "Couldn't unblock. Try again.")
                        } finally {
                            busyId = null
                        }
                    }
                }) { Text(if (busyId == user.userId) "…" else "Unblock") }
            }
        }
    }
}
