package com.redvaultsystems.redvaultmessage.ui.channels

import android.Manifest
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.model.ChannelLink
import com.redvaultsystems.redvaultmessage.data.model.Channels
import com.redvaultsystems.redvaultmessage.ui.components.ChipRow
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.SearchBox
import com.redvaultsystems.redvaultmessage.ui.components.SecurityLine
import com.redvaultsystems.redvaultmessage.ui.components.TabHeader
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle

// Channels: a port of the web's screens/Channels.tsx. One-way and not
// end-to-end encrypted, by design and labelled so: anyone can read a public
// channel, and anyone with the link a private one. Premium members create
// (up to 5) and post; anyone can find and follow. Following stays on this
// phone, and reading goes out without the session cookie, so the server
// learns neither who follows nor who reads, and there's no limit on followers.

private enum class ChannelTab(val label: String) { Following("Following"), Discover("Discover"), Yours("Yours") }

private const val MAX_NAME = 80
private const val MAX_DESCRIPTION = 500

/** The Channels tab: Following (kept on this phone), Discover (the public directory) and Yours (Premium members make channels here). */
@Composable
fun ChannelsTab(channels: ChannelSession, premium: Boolean, onOpen: (ChannelLink) -> Unit) {
    val colors = RedvaultTheme.colors
    val followed by channels.followed.collectAsStateWithLifecycle()
    var tab by rememberSaveable { mutableStateOf(if (followed.isNotEmpty()) ChannelTab.Following else ChannelTab.Discover) }
    Column(Modifier.fillMaxSize().background(colors.bg)) {
        TabHeader("Channels")
        SecurityLine("Not end-to-end encrypted · follows stay on this phone", R.drawable.ui_globe)
        Text(
            "Channels aren't end-to-end encrypted like your chats: anyone can read a public channel, and anyone with the link can read a " +
                "private one. Who follows a channel is kept only on each person's device, so there's no limit on followers.",
            color = colors.text3,
            fontSize = 13.sp,
            modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 10.dp, bottom = 8.dp),
        )
        NotificationPrompt(channels)
        ChipRow(ChannelTab.entries.toList(), tab, { it.label }, { tab = it })
        Box(Modifier.weight(1f).padding(top = 8.dp)) {
            when (tab) {
                ChannelTab.Following -> Following(channels, onOpen, onDiscover = { tab = ChannelTab.Discover })
                ChannelTab.Discover -> Discover(channels, onOpen)
                ChannelTab.Yours -> Yours(channels, premium, onOpen)
            }
        }
    }
}

/**
 * Asks once for permission to show new-post notifications (Android 13+).
 * They're worked out on this phone while the app is running; the server
 * has no follower list to push from.
 */
@Composable
private fun NotificationPrompt(channels: ChannelSession) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val followed by channels.followed.collectAsStateWithLifecycle()
    var allowed by remember { mutableStateOf(ChannelNotifications.allowed(context)) }
    var dismissed by remember { mutableStateOf(channels.askedForNotifications) }
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted -> allowed = granted }
    if (allowed || dismissed || followed.isEmpty() || Build.VERSION.SDK_INT < 33) return
    Row(
        Modifier.padding(horizontal = 20.dp, vertical = 4.dp).fillMaxWidth().background(colors.surface, RoundedCornerShape(14.dp)).padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text("Get a notification when a channel you follow posts, while the app is running.", color = colors.text2, fontSize = 13.sp, modifier = Modifier.weight(1f))
        TextButton(onClick = {
            channels.askedForNotifications = true
            dismissed = true
            ask.launch(Manifest.permission.POST_NOTIFICATIONS)
        }) { Text("Turn on", color = colors.accent, fontWeight = FontWeight.SemiBold) }
        TextButton(onClick = {
            channels.askedForNotifications = true
            dismissed = true
        }) { Text("Not now", color = colors.text3) }
    }
}

@Composable
private fun Following(channels: ChannelSession, onOpen: (ChannelLink) -> Unit, onDiscover: () -> Unit) {
    val colors = RedvaultTheme.colors
    val followed by channels.followed.collectAsStateWithLifecycle()
    // id -> the channel, or null once it's known to be gone (or private without the link); missing while loading or unreachable.
    var loaded by remember { mutableStateOf<Map<String, ChannelDto?>>(emptyMap()) }
    val ids = followed.joinToString(",") { it.id + ":" + it.secret.orEmpty() }
    LaunchedEffect(ids) {
        val results = coroutineScope {
            followed.map { f ->
                async {
                    try {
                        f.id to channels.get(f.id, channels.accessFor(f.secret))
                    } catch (e: ApiException) {
                        if (e.status == 404) f.id to null else null
                    }
                }
            }.awaitAll()
        }
        loaded = results.filterNotNull().toMap()
        // Coming back to the list refreshes the badge too.
        channels.checkNow()
    }

    if (followed.isEmpty()) {
        Column(
            Modifier.fillMaxSize().padding(32.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterVertically),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text("You don't follow any channels yet.", color = colors.text2, fontSize = 14.sp, textAlign = TextAlign.Center)
            PillButton("Discover channels", onDiscover, style = PillStyle.Secondary, height = 44.dp)
        }
        return
    }
    LazyColumn(Modifier.fillMaxSize()) {
        items(followed, key = { it.id }) { f ->
            val channel = loaded[f.id]
            when {
                f.id in loaded && channel == null -> Row(
                    Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(f.name, color = colors.text, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
                        Text("This channel was deleted, or made private and you don't have its new link.", color = colors.text3, fontSize = 13.sp)
                    }
                    TextButton(onClick = { channels.unfollow(f.id) }) { Text("Remove", color = colors.accent) }
                }
                channel != null -> ChannelRow(channels, channel, unread = Channels.hasUnread(f, channel), access = channels.accessFor(f.secret)) {
                    onOpen(ChannelLink(f.id, f.secret))
                }
                else -> ChannelRow(channels, ChannelDto(id = f.id, name = f.name, isPrivate = f.secret != null), unread = false, access = null) {
                    onOpen(ChannelLink(f.id, f.secret))
                }
            }
        }
    }
}

@Composable
private fun Discover(channels: ChannelSession, onOpen: (ChannelLink) -> Unit) {
    val scope = rememberCoroutineScope()
    var query by rememberSaveable { mutableStateOf("") }
    var results by remember { mutableStateOf<List<ChannelDto>?>(null) }
    var more by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var pasted by rememberSaveable { mutableStateOf("") }
    var linkProblem by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(query) {
        delay(250)
        try {
            val found = channels.search(query)
            results = found
            more = found.size == ChannelSession.PAGE
            error = null
        } catch (e: ApiException) {
            error = "Couldn't load channels. Try again."
        }
    }

    LazyColumn(Modifier.fillMaxSize()) {
        item {
            SearchBox(query, { query = it }, "Search channels", R.drawable.ui_search, Modifier.padding(horizontal = 20.dp, vertical = 4.dp))
        }
        error?.let { item { Box(Modifier.padding(horizontal = 20.dp, vertical = 6.dp)) { Notice(it) } } }
        val list = results
        when {
            list == null && error == null -> item { Hint("Loading…") }
            list != null && list.isEmpty() -> item { Hint(if (query.isNotBlank()) "No channels match." else "No channels yet.") }
        }
        items(list.orEmpty(), key = { it.id }) { channel -> ChannelRow(channels, channel, access = null) { onOpen(ChannelLink(channel.id, null)) } }
        if (more) {
            item {
                Box(Modifier.fillMaxWidth().padding(16.dp), contentAlignment = Alignment.Center) {
                    PillButton("Show more", {
                        scope.launch {
                            val shown = results.orEmpty()
                            val next = runCatching { channels.search(query, shown.size) }.getOrDefault(emptyList())
                            results = shown + next.filter { n -> shown.none { it.id == n.id } }
                            more = next.size == ChannelSession.PAGE
                        }
                    }, style = PillStyle.Secondary, height = 40.dp)
                }
            }
        }
        // Private channels aren't in the directory: they open from their link.
        item {
            Column(Modifier.padding(horizontal = 20.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Have a channel's link? Private channels open only from theirs.", color = RedvaultTheme.colors.text3, fontSize = 13.sp)
                DsTextField(pasted, { pasted = it; linkProblem = null }, "Paste a channel link")
                linkProblem?.let { Notice(it) }
                PillButton("Open link", {
                    val link = Channels.parse(pasted)
                    if (link == null) linkProblem = "That isn't a channel link." else {
                        pasted = ""
                        onOpen(link)
                    }
                }, enabled = pasted.isNotBlank(), style = PillStyle.Secondary, height = 40.dp, icon = R.drawable.ui_link)
            }
        }
    }
}

@Composable
private fun Yours(channels: ChannelSession, premium: Boolean, onOpen: (ChannelLink) -> Unit) {
    val colors = RedvaultTheme.colors
    val scope = rememberCoroutineScope()
    var mine by remember { mutableStateOf<List<ChannelDto>?>(null) }
    var creating by rememberSaveable { mutableStateOf(false) }
    var name by rememberSaveable { mutableStateOf("") }
    var description by rememberSaveable { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) {
        mine = runCatching { channels.mine() }.getOrDefault(emptyList())
    }

    LazyColumn(Modifier.fillMaxSize()) {
        item {
            Column(Modifier.padding(horizontal = 20.dp, vertical = 4.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                when {
                    // Google Play: no upgrade button here, just what the plan allows.
                    !premium -> Text(
                        "Creating a channel needs Premium. Anyone can follow channels on any plan.",
                        color = colors.text2,
                        fontSize = 14.sp,
                    )
                    creating -> {
                        DsTextField(name, { if (it.length <= MAX_NAME) name = it; error = null }, "Channel name")
                        DsTextField(
                            description,
                            { if (it.length <= MAX_DESCRIPTION) description = it; error = null },
                            "What's it about? (optional)",
                            singleLine = false,
                            minHeight = 90.dp,
                        )
                        Text(
                            "Your channel starts public: anyone can find it and read everything you post, with no limit on followers. " +
                                "You can make it private later. Posts show the channel's name and photo, never who runs it.",
                            color = colors.text3,
                            fontSize = 13.sp,
                        )
                        error?.let { Notice(it) }
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            PillButton(if (busy) "Creating…" else "Create channel", {
                                busy = true
                                error = null
                                scope.launch {
                                    try {
                                        val channel = channels.create(name, description)
                                        creating = false
                                        name = ""
                                        description = ""
                                        onOpen(ChannelLink(channel.id, null))
                                    } catch (e: Exception) {
                                        error = describeError(e, "Couldn't create the channel. Try again.")
                                    } finally {
                                        busy = false
                                    }
                                }
                            }, enabled = name.isNotBlank() && !busy, busy = busy, height = 44.dp)
                            PillButton("Cancel", { creating = false }, style = PillStyle.Secondary, enabled = !busy, height = 44.dp)
                        }
                    }
                    else -> PillButton("New channel", { creating = true }, Modifier.fillMaxWidth(), height = 46.dp, icon = R.drawable.ui_plus)
                }
                when {
                    mine == null -> Text("Loading…", color = colors.text3, fontSize = 14.sp)
                    mine.orEmpty().isEmpty() && premium && !creating -> Text("You haven't made a channel yet.", color = colors.text3, fontSize = 14.sp)
                }
            }
        }
        items(mine.orEmpty(), key = { it.id }) { channel ->
            // A private channel's photo needs its link's token; this phone unseals the secret.
            val secret = remember(channel.secretSealed) { if (channel.isPrivate) channels.ownSecret(channel) else null }
            ChannelRow(channels, channel, access = channels.accessFor(secret)) { onOpen(ChannelLink(channel.id, secret)) }
        }
    }
}

@Composable
private fun ChannelRow(channels: ChannelSession, channel: ChannelDto, unread: Boolean = false, access: String?, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(role = Role.Button, onClick = onClick)
            .heightIn(min = 64.dp)
            .padding(horizontal = 20.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        ChannelAvatar(channels, channel.name, channel.avatarUrl, access, size = 40.dp)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    channel.name,
                    color = colors.text,
                    fontSize = 16.sp,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f, fill = false),
                )
                if (channel.isPrivate) {
                    Text(
                        "Private",
                        color = colors.text2,
                        fontSize = 11.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.clip(RoundedCornerShape(6.dp)).background(colors.surface2).padding(horizontal = 6.dp, vertical = 2.dp),
                    )
                }
                if (unread) Box(Modifier.size(8.dp).clip(CircleShape).background(colors.accent).semantics { contentDescription = "New posts" })
            }
            if (channel.description.isNotEmpty()) {
                Text(channel.description, color = colors.text3, fontSize = 13.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
        }
        Text(channel.lastPostAt?.let(::day) ?: "No posts yet", color = colors.text3, fontSize = 12.sp)
    }
}

@Composable
private fun Hint(text: String) {
    Text(text, color = RedvaultTheme.colors.text3, fontSize = 14.sp, modifier = Modifier.padding(horizontal = 20.dp, vertical = 10.dp))
}

private fun day(iso: String): String =
    runCatching { DateTimeFormatter.ofLocalizedDate(FormatStyle.MEDIUM).format(Instant.parse(iso).atZone(ZoneId.systemDefault())) }.getOrDefault("")
