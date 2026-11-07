package com.redvaultsystems.redvaultmessage.ui.friends

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import com.redvaultsystems.redvaultmessage.data.ContactPreview
import com.redvaultsystems.redvaultmessage.data.FriendRequestView
import com.redvaultsystems.redvaultmessage.data.FriendRequests
import com.redvaultsystems.redvaultmessage.ui.account.AccountSession
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.NoticeTone
import com.redvaultsystems.redvaultmessage.ui.components.PrimaryButton
import com.redvaultsystems.redvaultmessage.ui.components.SectionLabel
import com.redvaultsystems.redvaultmessage.ui.components.SubScreen
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.launch

/** Port of ui/src/screens/NewContact.tsx: look a handle up, preview it, send a friend request. */
@Composable
fun AddFriendScreen(account: AccountSession, onBack: () -> Unit, onOpenRequests: () -> Unit, initialHandle: String = "") {
    val scope = rememberCoroutineScope()
    var handle by rememberSaveable { mutableStateOf(initialHandle) }
    var preview by remember { mutableStateOf<ContactPreview?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var sentTo by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }

    fun find() {
        if (handle.isBlank() || busy) return
        busy = true
        error = null
        sentTo = null
        preview = null
        scope.launch {
            try {
                preview = account.lookup(handle)
            } catch (e: Exception) {
                error = describeError(e, "No one with that handle.")
            } finally {
                busy = false
            }
        }
    }

    // A scanned contact code: look it up straight away.
    LaunchedEffect(initialHandle) { if (initialHandle.isNotBlank()) find() }

    SubScreen(title = "Add a friend", onBack = onBack) {
        Text(
            "Enter their exact handle. They'll get a friend request, and you can chat once they accept.",
            color = RedvaultTheme.colors.text2,
            style = MaterialTheme.typography.bodyMedium,
        )
        OutlinedTextField(
            value = handle,
            onValueChange = {
                handle = it
                preview = null
                error = null
            },
            label = { Text("Handle") },
            singleLine = true,
            keyboardOptions = KeyboardOptions(
                capitalization = KeyboardCapitalization.None,
                autoCorrectEnabled = false,
                imeAction = ImeAction.Search,
            ),
            keyboardActions = KeyboardActions(onSearch = { find() }),
            modifier = Modifier.fillMaxWidth(),
        )
        PrimaryButton(text = if (busy && preview == null) "Looking up…" else "Find", enabled = handle.isNotBlank(), busy = busy && preview == null, onClick = ::find)

        error?.let { Notice(it) }
        sentTo?.let {
            Notice("Friend request sent to $it. The chat appears once they accept.", NoticeTone.Success)
            TextButton(onClick = onOpenRequests) { Text("See pending requests") }
        }

        preview?.let { contact ->
            ContactCard(contact.handle, contact.avatarUrl) {
                when {
                    contact.isSelf -> Text("That's you.", color = RedvaultTheme.colors.text3)
                    contact.deleted -> Text("This account was deleted.", color = RedvaultTheme.colors.text3)
                    sentTo == contact.handle -> Text("Request sent", color = RedvaultTheme.colors.accent)
                    else -> OutlinedButton(
                        enabled = !busy,
                        onClick = {
                            busy = true
                            error = null
                            scope.launch {
                                try {
                                    account.sendFriendRequest(contact.handle)
                                    sentTo = contact.handle
                                } catch (e: Exception) {
                                    error = describeError(e, "Couldn't send that friend request.")
                                } finally {
                                    busy = false
                                }
                            }
                        },
                    ) { Text("Send request") }
                }
            }
        }
    }
}

/** Port of ui/src/screens/FriendRequests.tsx. Accepting opens the new chat. */
@Composable
fun FriendRequestsScreen(
    account: AccountSession,
    requests: FriendRequests,
    onBack: () -> Unit,
    onAddFriend: () -> Unit,
    onChatCreated: (conversationId: String) -> Unit,
) {
    val scope = rememberCoroutineScope()
    var busyId by remember { mutableStateOf<String?>(null) }
    var error by remember { mutableStateOf<String?>(null) }

    fun act(request: FriendRequestView, fallback: String, action: suspend () -> Unit) {
        busyId = request.id
        error = null
        scope.launch {
            try {
                action()
            } catch (e: Exception) {
                error = describeError(e, fallback)
            } finally {
                busyId = null
            }
        }
    }

    SubScreen(title = "Friend requests", onBack = onBack) {
        error?.let { Notice(it) }

        SectionLabel("Received")
        if (requests.incoming.isEmpty()) {
            Text("No pending requests.", color = RedvaultTheme.colors.text3, style = MaterialTheme.typography.bodyMedium)
        }
        requests.incoming.forEach { request ->
            ContactCard(request.handle, request.avatarUrl) {
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    TextButton(enabled = busyId == null, onClick = {
                        act(request, "Couldn't deny that request.") { account.deny(request) }
                    }) { Text("Deny", color = RedvaultTheme.colors.text2) }
                    OutlinedButton(enabled = busyId == null, onClick = {
                        act(request, "Couldn't accept that request.") { onChatCreated(account.accept(request)) }
                    }) { Text(if (busyId == request.id) "Accepting…" else "Accept") }
                }
            }
        }

        SectionLabel("Sent")
        if (requests.outgoing.isEmpty()) {
            Text("You haven't sent any pending requests.", color = RedvaultTheme.colors.text3, style = MaterialTheme.typography.bodyMedium)
        }
        requests.outgoing.forEach { request ->
            ContactCard(request.handle, request.avatarUrl) {
                TextButton(enabled = busyId == null, onClick = {
                    act(request, "Couldn't cancel that request.") { account.cancel(request) }
                }) { Text("Cancel") }
            }
        }

        TextButton(onClick = onAddFriend) { Text("Add a friend") }
        Text(
            "Denying doesn't tell the other person.",
            color = RedvaultTheme.colors.text3,
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@Composable
private fun ContactCard(handle: String, avatarUrl: String?, actions: @Composable () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(colors.surface, RoundedCornerShape(12.dp))
            .padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Avatar(avatarUrl = avatarUrl, seed = handle, size = 40.dp)
        Column(Modifier.weight(1f)) {
            Text(handle, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Medium, color = colors.text)
        }
        actions()
    }
}
