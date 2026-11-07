package com.redvaultsystems.redvaultmessage.ui.channels

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.TextAutoSize
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
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
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.isTraversalGroup
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.data.AvatarJpeg
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import com.redvaultsystems.redvaultmessage.data.model.ChannelLink
import com.redvaultsystems.redvaultmessage.data.model.ChannelPostRef
import com.redvaultsystems.redvaultmessage.data.model.Channels
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.ui.TimeFormat
import com.redvaultsystems.redvaultmessage.ui.chats.DayHeader
import com.redvaultsystems.redvaultmessage.ui.chats.LoadOlderRow
import com.redvaultsystems.redvaultmessage.ui.components.DsSwitch
import com.redvaultsystems.redvaultmessage.ui.components.FilterChip
import com.redvaultsystems.redvaultmessage.ui.components.Notice
import com.redvaultsystems.redvaultmessage.ui.components.NoticeTone
import com.redvaultsystems.redvaultmessage.ui.components.PillButton
import com.redvaultsystems.redvaultmessage.ui.components.PillStyle
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import com.redvaultsystems.redvaultmessage.ui.files.DsTextField
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultFonts
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId

private const val MAX_NAME = 80
private const val MAX_DESCRIPTION = 500
private const val NOTICE_MS = 4_000L

private class Confirm(val title: String?, val text: String, val action: String, val danger: Boolean = true, val run: suspend () -> Unit)

/** What opens over the thread. */
private enum class ChannelPage { Info, Scheduled, Suggestions }

/**
 * One channel, laid out like a chat (the web and the desktop have the same
 * design): a chat's top bar, then every post since it began as an incoming
 * bubble, oldest at the top and opening at the newest, each shown as the
 * channel's (its name and photo, never who posted it) with its time and view
 * count. Under that the owner has a chat-style composer, and everyone else
 * a read-only bar.
 *
 * The bubbles are ChannelPostViews' own rather than a chat's message bubble,
 * because a channel has none of a chat's protections to show: no lock,
 * security panel or delivery marks. The header's subtitle says so.
 *
 * The channel-info page holds Follow, mute, Copy link, Share, Report and all
 * the owner's settings; a strip above the thread opens scheduled posts and
 * suggestions to review. [linkSecret] is a private channel's secret from the
 * link it was opened with.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ChannelScreen(
    channelId: String,
    linkSecret: String?,
    channels: ChannelSession,
    webAppBase: String,
    forwardTargets: List<Conversation>,
    onForward: (conversationId: String, text: String, ref: ChannelPostRef, onResult: (String?) -> Unit) -> Unit,
    onOpenChannel: (ChannelLink) -> Unit,
    onBack: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val appLock = LocalAppLock.current
    val scope = rememberCoroutineScope()
    val followed by channels.followed.collectAsStateWithLifecycle()
    var channel by remember(channelId) { mutableStateOf<ChannelDto?>(null) }
    var posts by remember(channelId) { mutableStateOf<List<ChannelPostDto>>(emptyList()) }
    var older by remember(channelId) { mutableStateOf(false) }
    var loadingOlder by remember(channelId) { mutableStateOf(false) }
    var mine by remember(channelId) { mutableStateOf(false) }
    /** A private channel's link secret: from the link just opened, a followed channel, or the owner's own copy. */
    var secret by remember(channelId) { mutableStateOf<String?>(null) }
    var scheduled by remember(channelId) { mutableStateOf<List<ChannelPostDto>>(emptyList()) }
    var editing by remember(channelId) { mutableStateOf<Pair<String, String>?>(null) }
    var forwarding by remember(channelId) { mutableStateOf<ChannelPostDto?>(null) }
    var actionsFor by remember(channelId) { mutableStateOf<ChannelPostDto?>(null) }
    var panel by remember(channelId) { mutableStateOf<ChannelPage?>(null) }
    /** Suggestions waiting for the owner, for the strip; null until read, or when they couldn't be. */
    var waitingSuggestions by remember(channelId) { mutableStateOf<Int?>(null) }
    var busy by remember { mutableStateOf(false) }
    var error by remember(channelId) { mutableStateOf<String?>(null) }
    var notice by remember(channelId) { mutableStateOf<String?>(null) }
    var confirm by remember { mutableStateOf<Confirm?>(null) }
    var reporting by remember { mutableStateOf(false) }
    var openLink by remember { mutableStateOf<String?>(null) }
    val isAdmin = channels.isSuperAdmin
    // The owner's tools: its owner, or a super admin (who still can't change who reads it).
    val canManage = mine || isAdmin
    val following = Channels.isFollowing(followed, channelId)
    val followEntry = followed.firstOrNull { it.id == channelId }
    val access = channels.accessFor(secret)
    val focus = LocalFocusManager.current
    val listState = rememberSaveable(channelId, saver = LazyListState.Saver) { LazyListState() }
    // Kept here, so a half-written post survives the screen around it changing.
    val composer = rememberComposerState(channels, channelId)
    val suggestDraft = rememberSuggestState(channels, channelId)
    var suggestOpen by suggestDraft.open
    val today = LocalDate.now()
    // Newest first, with a header above each day's first post.
    val rows = remember(posts, today) {
        val zone = ZoneId.systemDefault()
        ChannelThread.rows(posts, { ChannelThread.dayOf(it.createdAt, zone) }) { TimeFormat.dayLabel(it, today) }
    }

    fun openPage(page: ChannelPage) {
        // The keyboard would otherwise keep typing into the composer hidden behind the page.
        focus.clearFocus()
        panel = page
    }

    BackHandler(enabled = panel != null) { panel = null }
    BackHandler(enabled = panel == null && suggestOpen) { suggestOpen = false }

    // A success note clears itself; a problem stays until the next action, or a tap.
    LaunchedEffect(notice) {
        if (notice != null) {
            delay(NOTICE_MS)
            notice = null
        }
    }

    // No notifications for the channel on screen.
    DisposableEffect(channelId) {
        channels.currentChannelId = channelId
        onDispose { if (channels.currentChannelId == channelId) channels.currentChannelId = null }
    }

    LaunchedEffect(channelId, linkSecret) {
        channel = null
        error = null
        val own = runCatching { channels.mine() }.getOrNull()?.firstOrNull { it.id == channelId }
        val ownSecret = own?.let { withContext(Dispatchers.Default) { channels.ownSecret(it) } }
        val known = linkSecret?.takeIf { it.isNotEmpty() } ?: ownSecret ?: channels.followedEntry(channelId)?.secret
        val token = channels.accessFor(known)
        try {
            val found = channels.get(channelId, token)
            val firstPage = channels.posts(channelId, null, token)
            mine = own != null
            if (own != null || isAdmin) {
                scheduled = runCatching { channels.scheduled(channelId) }.getOrDefault(emptyList())
                if (found.acceptSuggestions) waitingSuggestions = runCatching { channels.suggestions(channelId).size }.getOrNull()
            }
            secret = if (found.isPrivate) known else null
            channel = found
            posts = firstPage
            older = firstPage.size == ChannelSession.PAGE
            channels.countViews(firstPage.map { it.id })
        } catch (e: ApiException) {
            error = if (e.status == 404) "${e.message} A private channel opens only from its private link." else "Couldn't load this channel. Try again."
        }
    }

    // Opening a followed channel counts its posts as seen (and keeps the private link it was opened with).
    LaunchedEffect(channel, secret, following) {
        channel?.let { if (following) channels.markSeen(it, secret) }
    }

    fun run(fallback: String, action: suspend () -> Unit) {
        busy = true
        error = null
        notice = null
        scope.launch {
            try {
                action()
            } catch (e: Exception) {
                error = describeError(e, fallback)
            } finally {
                busy = false
            }
        }
    }

    /** A post from the composer (or an approved suggestion): at the bottom, in view, or with the scheduled ones. */
    fun added(post: ChannelPostDto, isScheduled: Boolean) {
        val shown = channel ?: return
        if (isScheduled) {
            scheduled = (scheduled + post).sortedBy { it.createdAt }
            notice = "Scheduled for ${runCatching { whenText(Instant.parse(post.createdAt)) }.getOrDefault(post.createdAt)}."
            return
        }
        posts = listOf(post) + posts
        channel = shown.copy(lastPostAt = post.createdAt)
        channels.countViews(listOf(post.id))
        // A keyed list keeps the row it was showing, which would leave the new post out of sight below it.
        listState.requestScrollToItem(0)
    }

    fun loadOlder() {
        val last = posts.lastOrNull() ?: return
        if (loadingOlder || !older) return
        loadingOlder = true
        scope.launch {
            try {
                val page = channels.posts(channelId, last.createdAt, access)
                posts = ChannelThread.appendOlder(posts, page)
                older = page.size == ChannelSession.PAGE
                channels.countViews(page.map { it.id })
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                // Older stays on, so the row can be tapped to try again.
                error = describeError(e, "Couldn't load older posts.")
            } finally {
                loadingOlder = false
            }
        }
    }

    /** Private: a fresh link (the old one stops working). Public: no link needed. */
    suspend fun setPrivate(makePrivate: Boolean) {
        val (updated, fresh) = channels.setPrivate(channelId, makePrivate)
        secret = fresh
        channel = updated
    }

    val pickPhoto = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        run("Couldn't use that photo.") {
            val image = withContext(Dispatchers.IO) { AvatarJpeg.base64(context.contentResolver, uri) }
            channel = channels.setPhoto(channelId, image)
        }
    }

    fun handleLink(url: String) {
        val link = Channels.parseOwn(url, webAppBase)
        if (link != null) onOpenChannel(link) else openLink = url
    }

    val shown = channel
    val link = when {
        shown == null -> null
        shown.isPrivate -> secret?.let { Channels.url(webAppBase, shown.id, it) }
        else -> Channels.url(webAppBase, shown.id)
    }

    /** The channel-info page: what a channel's header held, and all the owner's settings. */
    @Composable
    fun ChannelInfo(shown: ChannelDto) {
        val edit = editing
        if (edit != null) {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                DsTextField(edit.first, { if (it.length <= MAX_NAME) editing = it to edit.second }, "Channel name")
                DsTextField(edit.second, { if (it.length <= MAX_DESCRIPTION) editing = edit.first to it }, "Description", singleLine = false, minHeight = 90.dp)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    PillButton("Save", {
                        run("Couldn't save the changes.") {
                            channel = channels.update(channelId, edit.first, edit.second)
                            editing = null
                        }
                    }, enabled = edit.first.isNotBlank() && !busy, busy = busy, height = 48.dp)
                    PillButton("Cancel", { editing = null }, style = PillStyle.Secondary, enabled = !busy, height = 48.dp)
                }
            }
        } else {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                ChannelAvatar(channels, shown.name, shown.avatarUrl, access, size = 64.dp)
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(shown.name, fontFamily = RedvaultFonts.display, fontSize = 22.sp, fontWeight = FontWeight.Bold, color = colors.text)
                    Text(ChannelThread.subtitle(shown.isPrivate), color = colors.text3, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                    if (shown.isPrivate) Text("Only people with its link can read it.", color = colors.text3, fontSize = 12.sp)
                }
            }
            if (shown.description.isNotEmpty()) Text(shown.description, color = colors.text2, fontSize = 15.sp)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                PillButton(
                    if (following) "Following" else "Follow",
                    { if (following) channels.unfollow(channelId) else channels.follow(shown, secret) },
                    style = if (following) PillStyle.Secondary else PillStyle.Primary,
                    height = 48.dp,
                )
                if (following) {
                    val muted = followEntry?.muted == true
                    PillButton(
                        if (muted) "Muted" else "Notifications on",
                        { channels.setMuted(channelId, !muted) },
                        style = PillStyle.Secondary,
                        height = 48.dp,
                        icon = if (muted) R.drawable.ui_bell_off else R.drawable.ui_bell,
                    )
                }
                if (link != null) {
                    PillButton("Copy link", {
                        SecretClipboard.copy(context, link, secret = false)
                        notice = "Link copied."
                    }, style = PillStyle.Secondary, height = 48.dp, icon = R.drawable.ui_copy)
                    PillButton("Share", {
                        appLock?.expectExternalReturn()
                        share(context, link)
                    }, style = PillStyle.Secondary, height = 48.dp, icon = R.drawable.ui_share)
                }
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                if (canManage) {
                    TextButton(enabled = !busy, onClick = { editing = shown.name to shown.description }) { Text("Edit", color = colors.accent) }
                    TextButton(enabled = !busy, onClick = {
                        appLock?.expectExternalReturn()
                        pickPhoto.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                    }) { Text(if (shown.avatarUrl != null) "Change photo" else "Add photo", color = colors.accent) }
                    if (shown.avatarUrl != null) {
                        TextButton(enabled = !busy, onClick = {
                            run("Couldn't remove the photo.") {
                                channels.removePhoto(channelId)
                                channel = shown.copy(avatarUrl = null)
                            }
                        }) { Text("Remove photo", color = colors.text2) }
                    }
                } else {
                    TextButton(enabled = !busy, onClick = { reporting = true }) { Text("Report", color = colors.text2) }
                }
                if (canManage) {
                    TextButton(enabled = !busy, onClick = {
                        confirm = Confirm(null, "Delete “${shown.name}” and all its posts? This can't be undone.", "Delete") {
                            channels.delete(channelId)
                            onBack()
                        }
                    }) { Text("Delete channel", color = colors.danger) }
                }
            }
        }
        if (canManage) {
            Column(
                Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(16.dp)).padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                if (mine) {
                    ToggleRow(
                        "Private channel",
                        if (shown.isPrivate) "Not in Discover. Only people with the private link can read it." else "Off: anyone can find it in Discover and read it.",
                        shown.isPrivate,
                        enabled = !busy,
                    ) { on ->
                        confirm = if (on) {
                            Confirm(
                                "Make this channel private?",
                                "It leaves Discover, and only people with its private link can read it. Followers who don't have that link lose access.",
                                "Make private",
                                danger = false,
                            ) { setPrivate(true) }
                        } else {
                            Confirm("Make this channel public?", "Anyone can find it in Discover and read every post.", "Make public", danger = false) { setPrivate(false) }
                        }
                    }
                }
                ToggleRow(
                    "Suggested posts",
                    if (shown.acceptSuggestions) "Followers can send you posts to review. You see the post, never who sent it." else "Off: only you post in this channel.",
                    shown.acceptSuggestions,
                    enabled = !busy,
                ) { on -> run("Couldn't change that setting.") { channel = channels.setAcceptSuggestions(channelId, on) } }
                if (mine && shown.isPrivate && link != null) {
                    SelectionContainer {
                        Text(
                            link,
                            color = colors.text2,
                            fontSize = 13.sp,
                            fontFamily = RedvaultFonts.mono,
                            modifier = Modifier.fillMaxWidth().background(colors.surface2, RoundedCornerShape(10.dp)).padding(10.dp),
                        )
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        PillButton("Copy", {
                            SecretClipboard.copy(context, link, secret = false)
                            notice = "Link copied."
                        }, height = 48.dp, icon = R.drawable.ui_copy)
                        PillButton("Reset link", {
                            confirm = Confirm(
                                "Reset the private link?",
                                "Everyone who has the current link, followers included, loses access until you share the new one.",
                                "Reset",
                            ) { setPrivate(true) }
                        }, style = PillStyle.Secondary, enabled = !busy, height = 48.dp)
                    }
                }
            }
        }
    }

    /** A post waiting for its time, with Publish now and Delete. */
    @Composable
    fun ScheduledPost(shown: ChannelDto, post: ChannelPostDto) {
        Column(
            Modifier.fillMaxWidth().background(colors.surface, RoundedCornerShape(16.dp)).padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(
                "Scheduled for ${runCatching { whenText(Instant.parse(post.createdAt)) }.getOrDefault(post.createdAt)}",
                color = colors.accent,
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
            )
            PostContent(channels, shown.id, post, access, isOwner = true, onOpenLink = ::handleLink)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                PillButton("Publish now", {
                    run("Couldn't publish it.") {
                        val published = channels.publishNow(shown.id, post.id)
                        scheduled = scheduled.filterNot { it.id == post.id }
                        added(published, false)
                    }
                }, style = PillStyle.Secondary, enabled = !busy, height = 48.dp)
                TextButton(enabled = !busy, onClick = {
                    confirm = Confirm(null, "Delete this scheduled post?", "Delete") {
                        channels.deletePost(shown.id, post.id)
                        scheduled = scheduled.filterNot { it.id == post.id }
                    }
                }) { Text("Delete", color = colors.danger) }
            }
        }
    }

    val strip = if (canManage && shown != null) ChannelThread.ownerStrip(scheduled.size, shown.acceptSuggestions, waitingSuggestions) else emptyList()
    // Nothing left in the page that was open: back to the thread.
    LaunchedEffect(panel, scheduled.isEmpty()) {
        if (panel == ChannelPage.Scheduled && scheduled.isEmpty()) panel = null
    }

    @Composable
    fun messages() {
        val success = notice
        val problem = error
        if (success != null) Notice(success, NoticeTone.Success)
        if (problem != null) {
            Box(Modifier.clip(RoundedCornerShape(12.dp)).clickable(onClickLabel = "Dismiss") { error = null }) { Notice(problem) }
        }
    }

    Box(Modifier.fillMaxSize().background(colors.bg).statusBarsPadding().navigationBarsPadding().imePadding()) {
        // Under a page the thread is out of sight, so TalkBack mustn't reach it either.
        Column(Modifier.fillMaxSize().then(if (panel != null && shown != null) Modifier.clearAndSetSemantics {} else Modifier)) {
            ChannelBar(
                title = shown?.name ?: "Channel",
                subtitle = shown?.let { ChannelThread.subtitle(it.isPrivate) },
                onBack = onBack,
                onInfo = if (shown != null) ({ openPage(ChannelPage.Info) }) else null,
                avatar = shown?.let { { ChannelAvatar(channels, it.name, it.avatarUrl, access, size = 32.dp) } },
            )
            if (notice != null || error != null) {
                Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) { messages() }
            }
            if (strip.isNotEmpty()) {
                Row(
                    Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 6.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    strip.forEach { entry ->
                        FilterChip(
                            entry.label,
                            selected = false,
                            onClick = { openPage(if (entry.panel == OwnerPanel.Scheduled) ChannelPage.Scheduled else ChannelPage.Suggestions) },
                            icon = if (entry.panel == OwnerPanel.Scheduled) R.drawable.ui_clock else R.drawable.ui_chat,
                        )
                    }
                }
            }
            if (shown == null) {
                Box(Modifier.weight(1f).fillMaxWidth().padding(20.dp), contentAlignment = Alignment.Center) {
                    if (error == null) Text("Loading…", color = colors.text3)
                }
                return@Column
            }
            LazyColumn(
                state = listState,
                modifier = Modifier.weight(1f).fillMaxWidth(),
                reverseLayout = true,
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                if (rows.isEmpty()) {
                    item(key = "empty") {
                        Box(Modifier.fillParentMaxSize(), contentAlignment = Alignment.Center) {
                            Text("No posts yet.", color = colors.text3, fontSize = 14.sp)
                        }
                    }
                }
                items(rows, key = { it.key }, contentType = { if (it is ChannelRow.Day) 0 else 1 }) { row ->
                    when (row) {
                        is ChannelRow.Day -> DayHeader(row.label)
                        is ChannelRow.Post -> {
                            val post = row.post
                            ChannelPostBubble(
                                channels, shown, post, access, isOwner = canManage, onOpenLink = ::handleLink,
                                onChange = { changed -> posts = posts.map { if (it.id == changed.id) changed else it } },
                                onActions = if (ChannelThread.postActions(post, canManage, isAdmin).isNotEmpty()) ({ actionsFor = post }) else null,
                            )
                        }
                    }
                }
                // The top of what's loaded: older posts come in when it's reached (or tapped).
                if (older) {
                    item(key = "older", contentType = 2) {
                        LaunchedEffect(posts.size) { loadOlder() }
                        LoadOlderRow(loading = loadingOlder, onClick = ::loadOlder, label = "Load older posts")
                    }
                }
            }
            when (ChannelThread.bottomBar(canManage)) {
                ChannelBottomBar.Composer -> ChannelComposer(shown, composer, ::added)
                ChannelBottomBar.ReadOnly -> {
                    val canSuggest = ChannelThread.canSuggest(canManage, shown.acceptSuggestions)
                    // Their own column: a weight here would share the height with the thread and leave a gap.
                    Column(Modifier.fillMaxWidth()) {
                        if (canSuggest && suggestOpen) {
                            SuggestPost(
                                channels, shown, access, suggestDraft,
                                onSent = { notice = "Sent. The owner will look at it." },
                                modifier = Modifier.weight(1f, fill = false).heightIn(max = (LocalConfiguration.current.screenHeightDp * 0.5f).dp),
                            )
                        }
                        ChannelReaderBar(
                            following = following,
                            onToggleFollow = { if (following) channels.unfollow(channelId) else channels.follow(shown, secret) },
                            canSuggest = canSuggest,
                            onSuggest = { suggestOpen = !suggestOpen },
                        )
                    }
                }
            }
        }

        // A page over the thread rather than instead of it: what's playing or half-written underneath stays as it is.
        val page = panel
        if (page != null && shown != null) {
            Column(Modifier.fillMaxSize().background(colors.bg).pointerInput(Unit) {}.semantics { isTraversalGroup = true }) {
                ChannelBar(
                    title = when (page) {
                        ChannelPage.Info -> "Channel info"
                        ChannelPage.Scheduled -> "Scheduled (${scheduled.size})"
                        ChannelPage.Suggestions -> "Suggested posts"
                    },
                    subtitle = null,
                    onBack = { panel = null },
                )
                Column(
                    Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState()).padding(start = 20.dp, end = 20.dp, top = 8.dp, bottom = 24.dp),
                    verticalArrangement = Arrangement.spacedBy(14.dp),
                ) {
                    messages()
                    when (page) {
                        ChannelPage.Info -> ChannelInfo(shown)
                        ChannelPage.Scheduled -> scheduled.forEach { post -> ScheduledPost(shown, post) }
                        ChannelPage.Suggestions -> SuggestionReview(channels, shown.id, ::added, onCount = { waitingSuggestions = it })
                    }
                }
            }
        }
    }

    actionsFor?.let { post ->
        val actions = ChannelThread.postActions(post, canManage, isAdmin)
        AlertDialog(
            onDismissRequest = { actionsFor = null },
            containerColor = colors.surface,
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    if (PostAction.CopyText in actions) {
                        TextButton(onClick = {
                            actionsFor = null
                            SecretClipboard.copyPrivate(context, post.body)
                            notice = "Copied."
                        }) { Text("Copy text", color = colors.accent) }
                    }
                    if (PostAction.Forward in actions) {
                        TextButton(onClick = {
                            actionsFor = null
                            forwarding = post
                        }) { Text("Forward", color = colors.accent) }
                    }
                    if (PostAction.Delete in actions) {
                        TextButton(enabled = !busy, onClick = {
                            actionsFor = null
                            val from = shown ?: return@TextButton
                            confirm = Confirm(null, "Delete this post?", "Delete") {
                                channels.deletePost(from.id, post.id)
                                posts = posts.filterNot { it.id == post.id }
                            }
                        }) { Text("Delete", color = colors.danger) }
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { actionsFor = null }) { Text("Cancel", color = colors.text2) } },
        )
    }

    forwarding?.let { post ->
        val from = shown ?: return@let
        AlertDialog(
            onDismissRequest = { forwarding = null },
            containerColor = colors.surface,
            title = { Text("Forward to…", color = colors.text) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        "The copy goes into that chat end-to-end encrypted, as a post from ${from.name}. The channel itself isn't. Each copy that's seen counts as a view.",
                        color = colors.text2,
                        fontSize = 13.sp,
                    )
                    if (forwardTargets.isEmpty()) Text("You don't have a chat to forward to yet.", color = colors.text3, fontSize = 13.sp)
                    LazyColumn(Modifier.heightIn(max = 320.dp)) {
                        items(forwardTargets, key = { it.id }) { target ->
                            Text(
                                target.handle,
                                color = colors.text,
                                fontSize = 15.sp,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .heightIn(min = 48.dp)
                                    .clip(RoundedCornerShape(10.dp))
                                    .clickable(enabled = !busy, role = Role.Button) {
                                        busy = true
                                        onForward(target.id, post.body, ChannelPostRef(from.id, post.id, from.name)) { problem ->
                                            busy = false
                                            if (problem == null) {
                                                forwarding = null
                                                notice = "Forwarded."
                                            } else {
                                                error = problem
                                                forwarding = null
                                            }
                                        }
                                    }
                                    .padding(horizontal = 8.dp, vertical = 12.dp),
                            )
                        }
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = { forwarding = null }) { Text("Cancel", color = colors.text2) } },
        )
    }

    if (reporting) {
        var reason by remember { mutableStateOf("") }
        AlertDialog(
            onDismissRequest = { reporting = false },
            containerColor = colors.surface,
            title = { Text("Report this channel", color = colors.text) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text("What's wrong with this channel? Your report goes to the RedVault admins.", color = colors.text2, fontSize = 14.sp)
                    DsTextField(reason, { if (it.length <= 1000) reason = it }, "What's wrong", singleLine = false, minHeight = 90.dp)
                }
            },
            confirmButton = {
                TextButton(enabled = reason.isNotBlank(), onClick = {
                    reporting = false
                    run("Couldn't send the report.") {
                        channels.report(channelId, reason)
                        notice = "Thanks. The admins will look at it."
                    }
                }) { Text("Send report", color = colors.danger) }
            },
            dismissButton = { TextButton(onClick = { reporting = false }) { Text("Cancel", color = colors.text2) } },
        )
    }

    confirm?.let { ask ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            containerColor = colors.surface,
            title = if (ask.title != null) ({ Text(ask.title, color = colors.text) }) else null,
            text = { Text(ask.text, color = if (ask.title == null) colors.text else colors.text2) },
            confirmButton = {
                TextButton(onClick = {
                    confirm = null
                    run("That didn't work. Try again.", ask.run)
                }) { Text(ask.action, color = if (ask.danger) colors.danger else colors.accent) }
            },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("Cancel", color = colors.text2) } },
        )
    }

    openLink?.let { url ->
        AlertDialog(
            onDismissRequest = { openLink = null },
            containerColor = colors.surface,
            title = { Text("Open this link?", color = colors.text) },
            text = { Text(url, color = colors.text2, fontSize = 13.sp) },
            confirmButton = {
                TextButton(onClick = {
                    openLink = null
                    appLock?.expectExternalReturn()
                    runCatching { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
                }) { Text("Open", color = colors.accent) }
            },
            dismissButton = { TextButton(onClick = { openLink = null }) { Text("Cancel", color = colors.text2) } },
        )
    }
}

@Composable
private fun ToggleRow(title: String, description: String, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    val colors = RedvaultTheme.colors
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Column(Modifier.weight(1f)) {
            Text(title, color = colors.text, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
            Text(description, color = colors.text2, fontSize = 13.sp)
        }
        DsSwitch(checked, onChange, title, enabled = enabled)
    }
}

/**
 * A chat's top bar. The title opens the channel's info when there is any: its name never pushes the buttons
 * off, and the subtitle shrinks to stay whole on one line, because it's the screen's disclosure.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ChannelBar(title: String, subtitle: String?, onBack: () -> Unit, onInfo: (() -> Unit)? = null, avatar: (@Composable () -> Unit)? = null) {
    val colors = RedvaultTheme.colors
    TopAppBar(
        navigationIcon = {
            IconButton(onClick = onBack) { Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back", tint = colors.text) }
        },
        title = {
            Row(
                Modifier.fillMaxWidth().then(if (onInfo != null) Modifier.clickable(onClickLabel = "Channel info", role = Role.Button, onClick = onInfo) else Modifier),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                avatar?.invoke()
                Column(Modifier.weight(1f)) {
                    Text(title, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    if (subtitle != null) {
                        Text(
                            subtitle,
                            color = colors.text3,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            lineHeight = 16.sp,
                            autoSize = TextAutoSize.StepBased(minFontSize = 10.sp, maxFontSize = 12.sp, stepSize = 0.5.sp),
                        )
                    }
                }
            }
        },
        actions = {
            if (onInfo != null) {
                IconButton(onClick = onInfo) { Icon(painterResource(R.drawable.ui_info), contentDescription = "Channel info", tint = colors.text) }
            }
        },
        colors = TopAppBarDefaults.topAppBarColors(containerColor = colors.bg),
        windowInsets = WindowInsets(0),
    )
}

/** In place of the composer for everyone but the owner: there's nothing to type into here. */
@Composable
private fun ChannelReaderBar(following: Boolean, onToggleFollow: () -> Unit, canSuggest: Boolean, onSuggest: () -> Unit) {
    val colors = RedvaultTheme.colors
    @Composable
    fun followButton(modifier: Modifier = Modifier) = PillButton(
        if (following) "Following" else "Follow",
        onToggleFollow,
        modifier,
        style = if (following) PillStyle.Secondary else PillStyle.Primary,
        height = 48.dp,
    )
    Column(
        Modifier.fillMaxWidth().background(colors.surface).padding(horizontal = 16.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (canSuggest) {
            Text("Only the owner posts here.", color = colors.text3, fontSize = 13.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                followButton(Modifier.weight(1f))
                PillButton("Suggest a post", onSuggest, Modifier.weight(1f), style = PillStyle.Secondary, height = 48.dp)
            }
        } else {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("Only the owner posts here.", color = colors.text3, fontSize = 13.sp, modifier = Modifier.weight(1f))
                followButton()
            }
        }
    }
}

private fun share(context: android.content.Context, url: String) {
    val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, url)
    runCatching { context.startActivity(Intent.createChooser(send, "Share channel link")) }
}
