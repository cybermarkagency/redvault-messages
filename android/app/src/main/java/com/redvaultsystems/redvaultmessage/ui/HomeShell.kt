package com.redvaultsystems.redvaultmessage.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.ui.components.BottomTabBar
import com.redvaultsystems.redvaultmessage.ui.components.TabHeader
import com.redvaultsystems.redvaultmessage.ui.components.TabItem
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/** The tabs, in the design's order, with Channels after Contacts like the web's nav. */
internal object Tabs {
    const val CHATS = "chats"
    const val CALLS = "calls"
    const val CONTACTS = "contacts"
    const val CHANNELS = "channels"
    const val FILES = "files"
    const val PASSWORDS = "passwords"

    fun items(unreadChats: Int, unreadChannels: Int = 0, incomingFiles: Int = 0) = listOf(
        TabItem(CHATS, "Chats", R.drawable.ui_chat, badge = unreadChats),
        TabItem(CALLS, "Calls", R.drawable.ui_phone),
        TabItem(CONTACTS, "Contacts", R.drawable.ui_user),
        TabItem(CHANNELS, "Channels", R.drawable.ui_globe, badge = unreadChannels),
        TabItem(FILES, "Files", R.drawable.ui_cloud, badge = incomingFiles),
        TabItem(PASSWORDS, "Passwords", R.drawable.ui_key),
    )
}

/** A tab's page above the bottom tab bar. */
@Composable
internal fun HomeShell(
    tab: String,
    unreadChats: Int,
    unreadChannels: Int = 0,
    incomingFiles: Int = 0,
    onTab: (String) -> Unit,
    /** A strip above the page (Tor routing's status); null for none. */
    status: (@Composable () -> Unit)? = null,
    content: @Composable () -> Unit,
) {
    val colors = RedvaultTheme.colors
    Column(Modifier.fillMaxSize().background(colors.bg)) {
        Column(Modifier.weight(1f).fillMaxWidth().statusBarsPadding()) {
            status?.invoke()
            Box(Modifier.weight(1f).fillMaxWidth()) { content() }
        }
        BottomTabBar(Tabs.items(unreadChats, unreadChannels, incomingFiles), tab, onTab)
    }
}

/** A tab whose screens aren't in this build yet. */
@Composable
internal fun PendingTab(title: String, text: String) {
    val colors = RedvaultTheme.colors
    Column(Modifier.fillMaxSize()) {
        TabHeader(title)
        Column(
            Modifier.fillMaxSize().padding(32.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(text, color = colors.text3, fontSize = 14.sp, textAlign = TextAlign.Center)
        }
    }
}
