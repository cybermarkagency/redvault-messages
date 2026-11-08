// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.core.content.ContextCompat
import java.io.File
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.content.MediaType
import androidx.compose.foundation.content.ReceiveContentListener
import androidx.compose.foundation.content.consume
import androidx.compose.foundation.content.contentReceiver
import androidx.compose.foundation.content.hasMediaType
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.input.InputTransformation
import androidx.compose.foundation.text.input.TextFieldBuffer
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.TextFieldState
import androidx.compose.foundation.text.input.clearText
import androidx.compose.foundation.text.input.setTextAndPlaceCursorAtEnd
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.input.ImeAction
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.time.LocalDate
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.ui.semantics.Role
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.R
import com.redvaultsystems.redvaultmessage.calls.ActiveGroupCall
import com.redvaultsystems.redvaultmessage.calls.GroupCallLink
import com.redvaultsystems.redvaultmessage.crypto.GroupInviteLink
import com.redvaultsystems.redvaultmessage.data.model.ChannelLink
import com.redvaultsystems.redvaultmessage.data.model.Channels
import com.redvaultsystems.redvaultmessage.calls.GroupCallManager
import com.redvaultsystems.redvaultmessage.data.FileSendStage
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.data.model.Disappearing
import com.redvaultsystems.redvaultmessage.data.model.EditVersion
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.data.model.MessageFolding
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.data.model.NoticeKind
import com.redvaultsystems.redvaultmessage.data.model.PinnedView
import com.redvaultsystems.redvaultmessage.data.model.PollView
import com.redvaultsystems.redvaultmessage.data.model.RichMessage
import com.redvaultsystems.redvaultmessage.data.model.TextFormat
import com.redvaultsystems.redvaultmessage.data.model.VerificationStatus
import com.redvaultsystems.redvaultmessage.ui.TimeFormat
import com.redvaultsystems.redvaultmessage.ui.communities.CommunityReportDialog
import com.redvaultsystems.redvaultmessage.ui.components.Avatar
import com.redvaultsystems.redvaultmessage.ui.groups.GroupAvatar
import com.redvaultsystems.redvaultmessage.ui.components.describeError
import kotlinx.coroutines.launch
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme

/** Same wording as the server's pro_required refusal (FileService.requestUpload), which stays the real gate. */
private const val PRO_REQUIRED_MESSAGE = "Sending files isn't included in your plan."

/** 1 week, matching the server's RETENTION_WARNING_MS. */
private const val RETENTION_WARNING_MS = 7L * 24 * 60 * 60 * 1000

/** "@" then the start of a handle, right before the cursor (at the start or after a space). */
private val MENTION_PARTIAL = Regex("(?:^|\\s)@([^\\s@]*)$")

/** Own text messages can be edited for a day, like the web and desktop apps. */
private const val EDIT_WINDOW_MS = 24L * 60 * 60 * 1000

private fun canEdit(m: Message) =
    m.fromMe && m.kind == MessageKind.Text && !m.deleted && m.poll == null && m.notice == null && System.currentTimeMillis() - m.createdAtMs < EDIT_WINDOW_MS

/** Text that can be copied or selected: a plain message's (with its formatting markers, like the desktop's Copy text). */
private fun copyableText(m: Message): String? =
    m.text.takeIf { m.kind == MessageKind.Text && !m.deleted && m.poll == null && m.notice == null && it.isNotBlank() }

/** Port of ui/src/screens/Thread.tsx (text messages; files arrive in a later phase). */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ThreadScreen(
    conversation: Conversation,
    error: String?,
    /** Set when a send on device keys was refused and the person may use the older protocol for this chat instead. */
    onUseOlderProtocol: (() -> Unit)? = null,
    /** A device-key note that isn't an error (M5): which protocol the chat uses, or messages waiting on the server. */
    deviceKeysNote: String? = null,
    onBack: () -> Unit,
    onSend: (text: String, replyTo: Message?, onResult: (Boolean) -> Unit) -> Unit,
    onDelete: (messageId: String) -> Unit,
    onDeleteForMe: (messageId: String) -> Unit,
    onToggleReaction: (Message, emoji: String) -> Unit,
    onSetDisappearing: (Disappearing) -> Unit,
    onRequestVerification: (messageId: String) -> Unit,
    onOpenVerify: () -> Unit,
    onOpenGroupInfo: () -> Unit,
    onOpenDetails: (messageId: String) -> Unit,
    onReport: suspend (reason: String) -> Unit,
    /** Files from the message box, sent one after another; [onResult] gets how many went out and the first failure. */
    onSendFiles: (List<PendingFile>, onStage: (index: Int, FileSendStage) -> Unit, onResult: (sent: Int, error: String?) -> Unit) -> Unit,
    onSendVoice: (File, onStage: (FileSendStage) -> Unit, onResult: (String?) -> Unit) -> Unit,
    onDownloadFile: DownloadFile,
    /** Tor routing: a message the server refused goes out again, or a queued one is dropped unsent. */
    onRetryQueued: (id: String) -> Unit = {},
    onDiscardQueued: (id: String) -> Unit = {},
    /** Tor routing: what to say while Tor is down or messages are waiting (null: nothing to say). */
    torNotice: String? = null,
    /** Sending files is Pro-only; the server enforces it, this just says so up front. Opening received files is for everyone. */
    canSendFiles: Boolean,
    /** This user has blocked the other person. */
    blocked: Boolean,
    onSetBlocked: suspend (Boolean) -> Unit,
    /** The other person is on the live channel now (only known while you share your own status). */
    online: Boolean = false,
    /** Handles of people typing here right now (only while typing indicators are on). */
    typing: List<String> = emptyList(),
    /** Called on each edit of the message box; the live session throttles it. */
    onTyping: () -> Unit = {},
    /** Voice or video call this contact; null hides the buttons (groups, or calls unavailable). */
    onCall: ((kind: String) -> Unit)? = null,
    /** Start (or join) a group call; null hides the buttons (not a group, or group calls are off on this server). */
    onGroupCall: ((kind: String) -> Unit)? = null,
    /** A call going on in this group, for the Join banner. */
    ongoingGroupCall: ActiveGroupCall? = null,
    /** Only a Premium member can start a group call: the buttons are dimmed and say so, unless a call is going on (then you join it). */
    groupCallBlocked: Boolean = false,
    /** You're on a call already (this group's or another). */
    callBusy: Boolean = false,
    /** The call you're on is this group's: no Join banner for it. */
    inThisGroupCall: Boolean = false,
    /** The web app's address, which a group call's link in a message points at. */
    webAppBase: String = "",
    /** Joins the group call a tapped link in a message points at, inside the app. */
    onJoinGroupCallLink: ((callId: String) -> Unit)? = null,
    /** A group invite link to this app's own address, tapped in a message: open its join screen. */
    onOpenGroupInvite: ((linkId: String, secret: String) -> Unit)? = null,
    /** A channel link to this app's own address tapped in a message, or "Forwarded from" a channel: open the channel. */
    onOpenChannel: ((ChannelLink) -> Unit)? = null,
    /** A post forwarded from a channel came into view: it counts as a view of the post (once per phone). */
    onChannelPostShown: ((postId: String) -> Unit)? = null,
    /** Your own handle, so mentions of you stand out. */
    selfHandle: String,
    /** The other chats a message can be forwarded to. */
    forwardTargets: List<Conversation>,
    onEdit: (messageId: String, text: String, onResult: (Boolean) -> Unit) -> Unit,
    /** One or more messages to one or more chats; [onResult] gets null once all are sent, or a message that's safe to show. */
    onForward: (List<Message>, toConversationIds: List<String>, onResult: (String?) -> Unit) -> Unit,
    /** Several ticked messages: hidden on this phone, or (all your own) deleted for everyone. */
    onDeleteMany: (messageIds: Set<String>, forEveryone: Boolean) -> Unit = { _, _ -> },
    /** "Load older messages": [onDone] once they're in. */
    onLoadOlder: (onDone: () -> Unit) -> Unit = { it() },
    /** Pictures shown in their bubbles (null: every file is a Download bubble). */
    pictures: ChatPictures? = null,
    /** What was typed here and not sent, last time; [onDraftChange] gets it back on leaving the chat. */
    initialDraft: String = "",
    onDraftChange: (String) -> Unit = {},
    /** Files or text shared from another app into this chat: they wait in the message box until sent. */
    incomingShare: SharedItems? = null,
    onShareTaken: () -> Unit = {},
    /** A one-to-one chat's details: the contact, shared media, mute, favorite. */
    onOpenChatDetails: (() -> Unit)? = null,
    onOpenSharedMedia: (() -> Unit)? = null,
    onTogglePin: (Message) -> Unit,
    onUnpin: (messageId: String) -> Unit,
    onCreatePoll: (question: String, options: List<String>, multiple: Boolean, onResult: (String?) -> Unit) -> Unit,
    onVote: (poll: Message, options: List<Int>) -> Unit,
    onEndPoll: (pollId: String) -> Unit,
    /** Communities: the community's page, which the header and Info open instead of group info. */
    onOpenCommunity: (() -> Unit)? = null,
    /** Communities: reports a message to its moderators or RedVault; the reporter decides whether its text goes along. */
    onReportCommunityMessage: (suspend (Message, category: String, details: String, shareText: Boolean) -> Unit)? = null,
) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    val appLock = LocalAppLock.current
    val community = conversation.meta.community
    // A channel's subscribers read; only the creator and staff post (the server refuses anything else).
    val readOnly = community != null && !community.canPost
    var reportingMessage by remember { mutableStateOf<Message?>(null) }
    var showReport by remember { mutableStateOf(false) }
    var confirmBlock by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    var pendingDelete by remember { mutableStateOf<String?>(null) }
    var actionsFor by remember { mutableStateOf<Message?>(null) }
    var confirmOlder by remember { mutableStateOf(false) }
    var replyingTo by remember { mutableStateOf<Message?>(null) }
    var editing by remember { mutableStateOf<Message?>(null) }
    var forwarding by remember { mutableStateOf<List<Message>?>(null) }
    var creatingPoll by remember { mutableStateOf(false) }
    var versionsOf by remember { mutableStateOf<Message?>(null) }
    var reactingTo by remember { mutableStateOf<Message?>(null) }
    var openLink by remember { mutableStateOf<String?>(null) }
    var highlighted by remember { mutableStateOf<String?>(null) }
    var viewingPicture by remember { mutableStateOf<Message?>(null) }
    var selectingTextOf by remember { mutableStateOf<Message?>(null) }
    var loadingOlder by remember { mutableStateOf(false) }
    // Ticking messages to forward or delete several at once (the desktop's selection mode).
    var selecting by remember { mutableStateOf(false) }
    var selected by remember { mutableStateOf(emptySet<String>()) }
    var deletingMany by remember { mutableStateOf(false) }
    // Search in this chat: only the messages loaded on this phone.
    var searchOpen by remember { mutableStateOf(false) }
    var searchQuery by remember { mutableStateOf("") }
    var searchHit by remember { mutableStateOf<String?>(null) }
    val listState = rememberLazyListState()
    /** A group call button or the Join banner: join the one going on, start one if you can, or say why not. */
    fun placeGroupCall(kind: String) {
        val message = when {
            callBusy -> GroupCallManager.ALREADY_IN_CALL
            groupCallBlocked -> GroupCallManager.PREMIUM_ONLY
            else -> null
        }
        if (message != null) android.widget.Toast.makeText(context, message, android.widget.Toast.LENGTH_LONG).show() else onGroupCall?.invoke(kind)
    }
    val today = LocalDate.now()
    // Newest first, with a header above each day's first message.
    val rows = remember(conversation.messages, today) {
        ThreadRows.build(conversation.messages.asReversed(), TimeFormat::localDate) { TimeFormat.dayLabel(it, today) }
    }
    // A keyed list keeps the row that was at the bottom where it is when a newer one comes in, so the new message
    // stayed out of sight. Follow it when the old newest was at the bottom, and always after sending. Whether this
    // runs before the layout that adds the row (first visible is still 0) or after it (the kept row is first
    // visible), the first visible index is then at most the old newest's new index. The request is applied at
    // the next layout, over the kept position (a scroll before that layout would be to where the list already is).
    val newestRow = rows.firstOrNull()
    var previousNewestKey by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(newestRow?.key) {
        val before = previousNewestKey
        previousNewestKey = newestRow?.key
        if (before == null || newestRow == null) return@LaunchedEffect
        val mine = (newestRow as? ThreadRow.Item)?.message?.fromMe == true
        val beforeIndex = rows.indexOfFirst { it.key == before }
        if (mine || (beforeIndex >= 0 && listState.firstVisibleItemIndex <= beforeIndex)) listState.requestScrollToItem(0)
    }
    val pinned = remember(conversation.messages) { MessageFolding.pinnedFrom(conversation.messages) }
    // Only a group's members can be mentioned.
    val memberHandles = if (conversation.isGroup) conversation.meta.members.map { it.handle } else emptyList()
    val mentionCandidates = if (conversation.isGroup) conversation.meta.members.filter { !it.isSelf }.map { it.handle } else emptyList()

    /** Scrolls to a message (if it's loaded) and flashes it. */
    fun jumpTo(messageId: String) {
        val id = messageId.lowercase()
        val index = ThreadRows.indexOf(rows, id)
        if (index < 0) return
        scope.launch {
            listState.animateScrollToItem(index)
            highlighted = id
            kotlinx.coroutines.delay(1_500)
            if (highlighted == id) highlighted = null
        }
    }

    fun loadOlder() {
        if (loadingOlder || !conversation.hasOlder) return
        loadingOlder = true
        onLoadOlder { loadingOlder = false }
    }

    val searchHits = remember(conversation.messages, searchQuery) { ThreadSearch.hits(conversation.messages, searchQuery) }
    fun showHit(id: String?) {
        searchHit = id
        val index = id?.let { ThreadRows.indexOf(rows, it) } ?: -1
        if (index >= 0) scope.launch { listState.animateScrollToItem(index) }
    }
    // "Search older" with nothing found yet: show the newest match it brings in.
    var showOlderHit by remember { mutableStateOf(false) }
    // A new query starts at the newest match; new or older messages keep the current one if it still matches.
    LaunchedEffect(searchQuery) {
        showOlderHit = false
        if (searchQuery.isBlank()) {
            searchHit = null
            return@LaunchedEffect
        }
        kotlinx.coroutines.delay(200)
        showHit(ThreadSearch.hits(conversation.messages, searchQuery).lastOrNull())
    }
    LaunchedEffect(searchHits) {
        if (searchHit != null && searchHit !in searchHits) searchHit = searchHits.getOrNull(ThreadSearch.keep(searchHits, searchHit))
        if (showOlderHit && searchHit == null && searchHits.isNotEmpty()) {
            showOlderHit = false
            showHit(searchHits.last())
        }
    }
    fun stepSearch(step: Int) {
        if (searchHits.isEmpty()) return
        val from = searchHits.indexOf(searchHit)
        showHit(searchHits[if (from < 0) (if (step < 0) searchHits.lastIndex else 0) else ThreadSearch.step(from, searchHits.size, step)])
    }
    fun closeSearch() {
        searchOpen = false
        searchQuery = ""
        searchHit = null
    }
    fun stopSelecting() {
        selecting = false
        selected = emptySet()
    }
    fun toggleSelected(id: String) {
        selected = if (id in selected) selected - id else selected + id
    }
    BackHandler(enabled = selecting) { stopSelecting() }
    BackHandler(enabled = searchOpen && !selecting) { closeSearch() }

    fun whoDid(m: Message) = when {
        m.fromMe -> "You"
        conversation.isGroup -> conversation.meta.handleOf(m.senderId)
        else -> conversation.handle
    }

    // Shown to both participants whenever a live message here is within a
    // week of the 6-month retention cutoff.
    val earliestDeleteAt = conversation.messages.mapNotNull { it.deleteAtMs }.minOrNull()
    val showRetention = earliestDeleteAt != null && earliestDeleteAt - System.currentTimeMillis() <= RETENTION_WARNING_MS

    Scaffold(
        containerColor = colors.bg,
        topBar = {
            if (selecting) {
                // What you can do with the ticked messages.
                val forwardable = MessageSelection.forwardable(conversation.messages, selected)
                TopAppBar(
                    navigationIcon = {
                        IconButton(onClick = ::stopSelecting) { Icon(painterResource(R.drawable.ic_close), contentDescription = "Stop selecting") }
                    },
                    title = { Text(if (selected.isEmpty()) "Select messages" else "${selected.size} selected", fontWeight = FontWeight.SemiBold) },
                    actions = {
                        IconButton(enabled = forwardable.isNotEmpty(), onClick = { forwarding = forwardable }) {
                            Icon(painterResource(R.drawable.ui_share), contentDescription = "Forward", tint = if (forwardable.isNotEmpty()) colors.text else colors.text3)
                        }
                        IconButton(enabled = selected.isNotEmpty(), onClick = { deletingMany = true }) {
                            Icon(painterResource(R.drawable.ui_trash), contentDescription = "Delete", tint = if (selected.isNotEmpty()) colors.danger else colors.text3)
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(containerColor = colors.accentSoft),
                )
                return@Scaffold
            }
            TopAppBar(
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back") }
                },
                title = {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        Row(
                            Modifier.clickable(
                                enabled = conversation.isGroup || onOpenChatDetails != null,
                                onClickLabel = if (conversation.isGroup) "Group info" else "Chat details",
                                onClick = if (conversation.isGroup) onOpenCommunity ?: onOpenGroupInfo else onOpenChatDetails ?: {},
                            ),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(10.dp),
                        ) {
                            if (conversation.isGroup) {
                                GroupAvatar(conversation.handle, size = 32.dp)
                            } else {
                                Avatar(avatarUrl = conversation.avatarUrl, seed = conversation.handle, size = 32.dp, online = online)
                            }
                            Column {
                                Text(conversation.handle, fontWeight = FontWeight.SemiBold, maxLines = 1)
                                val status = when {
                                    typing.isNotEmpty() && conversation.isGroup ->
                                        if (typing.size == 1) "${typing[0]} is typing…" else "${typing.size} people are typing…"
                                    typing.isNotEmpty() -> "typing…"
                                    online && !conversation.isGroup -> "online"
                                    else -> null
                                }
                                // Their key changed: said under the name (after typing) until the new number is verified.
                                if (typing.isEmpty() && !conversation.isGroup && conversation.keyChanged) {
                                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                        Icon(painterResource(R.drawable.ic_alert), contentDescription = null, tint = colors.warn, modifier = Modifier.size(12.dp))
                                        Text("Safety number changed", color = colors.warn, fontSize = 12.sp, maxLines = 1)
                                    }
                                } else if (status != null) {
                                    Text(status, color = colors.accent, fontSize = 12.sp, maxLines = 1)
                                } else if (community != null) {
                                    Text(
                                        (if (community.isChannel) "Channel" else "Community") + " · end-to-end encrypted" + if (readOnly) " · read only" else "",
                                        color = colors.text3,
                                        fontSize = 12.sp,
                                        maxLines = 1,
                                    )
                                }
                            }
                        }
                    }
                },
                actions = {
                    if (conversation.isGroup) {
                        if (onGroupCall != null) {
                            val tint = if (groupCallBlocked || callBusy) colors.text3 else colors.text
                            IconButton(onClick = { placeGroupCall(ongoingGroupCall?.kind ?: "voice") }) {
                                Icon(
                                    painterResource(R.drawable.ui_phone),
                                    contentDescription = if (ongoingGroupCall != null) "Join the group call" else if (groupCallBlocked) "Group voice call. Only Premium members can start group calls" else "Group voice call",
                                    tint = tint,
                                )
                            }
                            IconButton(onClick = { placeGroupCall(ongoingGroupCall?.kind ?: "video") }) {
                                Icon(
                                    painterResource(R.drawable.ui_video),
                                    contentDescription = if (ongoingGroupCall != null) "Join the group call" else if (groupCallBlocked) "Group video call. Only Premium members can start group calls" else "Group video call",
                                    tint = tint,
                                )
                            }
                        }
                        TextButton(onClick = onOpenCommunity ?: onOpenGroupInfo) { Text("Info") }
                        GroupThreadMenu(onSearch = { searchOpen = true }, onSelect = { selecting = true })
                        return@TopAppBar
                    }
                    if (onCall != null && !blocked) {
                        IconButton(onClick = { onCall("voice") }) { Icon(painterResource(R.drawable.ui_phone), contentDescription = "Voice call", tint = colors.text) }
                        IconButton(onClick = { onCall("video") }) { Icon(painterResource(R.drawable.ui_video), contentDescription = "Video call", tint = colors.text) }
                    }
                    VerifiedChip(conversation.verified, onClick = onOpenVerify)
                    ThreadMenu(
                        blocked = blocked,
                        onSafetyNumber = onOpenVerify,
                        onReport = { showReport = true },
                        onToggleBlock = { confirmBlock = true },
                        onChatDetails = onOpenChatDetails,
                        onSharedMedia = onOpenSharedMedia,
                        onSearch = { searchOpen = true },
                        onSelect = { selecting = true },
                    )
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = colors.bg),
            )
        },
        bottomBar = {
            if (readOnly) {
                ReadOnlyBar(Modifier.navigationBarsPadding())
                return@Scaffold
            }
            Composer(
                onSend = { text, onResult ->
                    val target = editing
                    if (target == null) {
                        onSend(text, replyingTo) { sent ->
                            if (sent) replyingTo = null
                            onResult(sent)
                        }
                    } else if (text.trim() == target.text.trim()) {
                        // Nothing changed: nothing to send.
                        editing = null
                        onResult(true)
                    } else {
                        onEdit(target.id, text) { sent ->
                            if (sent) editing = null
                            onResult(sent)
                        }
                    }
                },
                replyingTo = replyingTo,
                otherHandle = conversation.handle,
                onCancelReply = { replyingTo = null },
                editing = editing,
                onCancelEdit = { editing = null },
                mentionCandidates = mentionCandidates,
                onStartPoll = { creatingPoll = true },
                onTyping = onTyping,
                initialDraft = initialDraft,
                onDraftChange = onDraftChange,
                incomingShare = incomingShare,
                onShareTaken = onShareTaken,
                onSendFiles = onSendFiles, onSendVoice = onSendVoice, canSendFiles = canSendFiles, modifier = Modifier.navigationBarsPadding().imePadding())
        },
    ) { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {
            if (searchOpen) {
                SearchBar(
                    query = searchQuery,
                    onQueryChange = { searchQuery = it },
                    position = searchHits.indexOf(searchHit),
                    count = searchHits.size,
                    onOlder = { stepSearch(-1) },
                    onNewer = { stepSearch(1) },
                    // Only what's loaded is searched: this loads older messages too.
                    onSearchOlder = if (conversation.hasOlder && searchQuery.isNotBlank()) ({ showOlderHit = true; loadOlder() }) else null,
                    searchingOlder = loadingOlder,
                    onClose = ::closeSearch,
                )
            }
            error?.let { ErrorBanner(it) }
            deviceKeysNote?.let { Banner(text = it) }
            if (onUseOlderProtocol != null) {
                Banner(
                    text = "This chat uses device keys. You can send it with the older protocol instead, without per-device keys.",
                    action = "Use the older protocol",
                    onClick = { confirmOlder = true },
                )
            }
            if (confirmOlder && onUseOlderProtocol != null) {
                AlertDialog(
                    onDismissRequest = { confirmOlder = false },
                    containerColor = colors.surface2,
                    title = { Text("Use the older protocol?", color = colors.text) },
                    text = {
                        Text(
                            "New messages in this chat will use the usual encryption instead of per-device keys. Messages already sent or received on device keys stay as they are. You can't undo this here.",
                            color = colors.text2,
                        )
                    },
                    confirmButton = { TextButton(onClick = { confirmOlder = false; onUseOlderProtocol() }) { Text("Use the older protocol", color = colors.warn) } },
                    dismissButton = { TextButton(onClick = { confirmOlder = false }) { Text("Cancel", color = colors.text2) } },
                )
            }
            torNotice?.let { Banner(text = it) }
            if (blocked) {
                Banner(
                    text = "You blocked ${conversation.handle}. Neither of you can send messages until you unblock them.",
                    action = "Unblock",
                    onClick = { scope.launch { runCatching { onSetBlocked(false) } } },
                )
            }
            // A key that failed its check isn't used: the chat says why, whatever else is going on.
            conversation.keyProblem?.let { Banner(text = it) }
            if (!conversation.isGroup && conversation.keyChanged) {
                // Their key changed: say so until the new number is verified.
                Banner(
                    text = "${conversation.handle}'s safety number changed. Someone may be intercepting. Sending is paused until you verify it.",
                    action = "Verify",
                    onClick = onOpenVerify,
                )
            } else if (!conversation.verified && !conversation.isGroup) {
                Banner(
                    text = "Not verified. Confirm this contact's safety number.",
                    action = "Verify",
                    onClick = onOpenVerify,
                )
            }
            if (conversation.isGroup && ongoingGroupCall != null && onGroupCall != null && !inThisGroupCall) {
                Banner(
                    text = "A group ${ongoingGroupCall.kind} call is going on" +
                        if (ongoingGroupCall.participants > 0) " · ${ongoingGroupCall.participants} in the call" else "",
                    action = "Join",
                    onClick = { placeGroupCall(ongoingGroupCall.kind) },
                    positive = true,
                )
            }
            if (showRetention) {
                Banner(
                    text = "A message here will be permanently deleted on ${TimeFormat.longDate(earliestDeleteAt)}.",
                )
            }
            // The timer is this user's own, for what they send: a channel's readers send nothing.
            if (!readOnly) DisappearingRow(conversation.disappearing, onSetDisappearing)
            if (pinned.isNotEmpty()) PinnedBar(pinned, onJump = ::jumpTo, onUnpin = onUnpin)

            LazyColumn(
                state = listState,
                modifier = Modifier.weight(1f).fillMaxWidth(),
                reverseLayout = true,
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                items(rows, key = { it.key }, contentType = { if (it is ThreadRow.Day) 0 else 1 }) { row ->
                    if (row is ThreadRow.Day) {
                        DayHeader(row.label)
                        return@items
                    }
                    val message = (row as ThreadRow.Item).message
                    val notice = message.notice
                    if (notice != null) {
                        NoticeLine(message, who = whoDid(message), onClick = { if (notice.kind == NoticeKind.Pin) jumpTo(notice.targetId) })
                        return@items
                    }
                    val selectable = selecting && MessageSelection.selectable(message)
                    Box {
                        MessageBubble(
                            message = message,
                            onRequestVerification = { onRequestVerification(message.id) },
                            onOpenDetails = { onOpenDetails(message.id) },
                            onDownloadFile = onDownloadFile,
                            // A queued message isn't on the server yet: nothing to reply to, react to or delete there.
                            // A deleted one has nothing left to act on.
                            onLongPress = { if (!message.queued && !message.deleted) actionsFor = message },
                            pictures = pictures,
                            onOpenPicture = { viewingPicture = message },
                            selected = if (selecting) message.id in selected else null,
                            onRetryQueued = { onRetryQueued(message.id) },
                            onDiscardQueued = { onDiscardQueued(message.id) },
                            senderName = if (conversation.isGroup && !message.fromMe) conversation.meta.handleOf(message.senderId) else null,
                            onToggleReaction = { emoji -> onToggleReaction(message, emoji) },
                            mentions = memberHandles,
                            selfHandle = selfHandle,
                            onOpenLink = { url ->
                                // This app's own links open right here: a group call's joins it, an invite opens
                                // its join screen, a channel opens the channel. Anything else is an ordinary link.
                                val own = webAppBase.isNotEmpty()
                                val callId = if (onJoinGroupCallLink != null && own) GroupCallLink.parseOwn(url, webAppBase) else null
                                val invite = if (onOpenGroupInvite != null && own) GroupInviteLink.parseOwn(url, webAppBase) else null
                                val channel = if (onOpenChannel != null && own) Channels.parseOwn(url, webAppBase) else null
                                when {
                                    callId != null -> onJoinGroupCallLink?.invoke(callId)
                                    invite != null -> onOpenGroupInvite?.invoke(invite.linkId, invite.secret)
                                    channel != null -> onOpenChannel?.invoke(channel)
                                    else -> openLink = url
                                }
                            },
                            onShowVersions = { versionsOf = message },
                            onJumpToReply = { message.replyTo?.let { jumpTo(it.messageId) } },
                            onVote = { options -> onVote(message, options) },
                            onEndPoll = if (message.fromMe) ({ onEndPoll(message.id) }) else null,
                            highlighted = highlighted == message.id.lowercase() || (searchOpen && searchHit == message.id),
                            onOpenForwardedChannel = message.channelPost?.let { ref -> onOpenChannel?.let { open -> { open(ChannelLink(ref.channelId, null)) } } },
                            onChannelPostShown = onChannelPostShown,
                        )
                        // Selecting: a tap anywhere on the row ticks it (links, files and reactions wait).
                        if (selectable) {
                            Box(
                                Modifier
                                    .matchParentSize()
                                    .combinedClickable(
                                        onClickLabel = if (message.id in selected) "Untick" else "Tick",
                                        onLongClick = { toggleSelected(message.id) },
                                        onClick = { toggleSelected(message.id) },
                                    ),
                            )
                        }
                    }
                }
                // The top of what's loaded: older messages come in when it's reached (or tapped).
                if (conversation.hasOlder) {
                    item(key = "older", contentType = 2) {
                        LaunchedEffect(conversation.messages.size) { loadOlder() }
                        LoadOlderRow(loading = loadingOlder, onClick = ::loadOlder)
                    }
                }
            }
        }
    }

    if (confirmBlock) {
        AlertDialog(
            onDismissRequest = { confirmBlock = false },
            title = { Text(if (blocked) "Unblock ${conversation.handle}?" else "Block ${conversation.handle}?") },
            text = {
                Text(
                    if (blocked) "You'll be able to message each other again."
                    else "Neither of you will be able to send messages or friend requests until you unblock them. They aren't told.",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmBlock = false
                    scope.launch { runCatching { onSetBlocked(!blocked) } }
                }) { Text(if (blocked) "Unblock" else "Block", color = if (blocked) colors.accent else colors.danger) }
            },
            dismissButton = { TextButton(onClick = { confirmBlock = false }) { Text("Cancel") } },
        )
    }

    if (showReport) {
        ReportDialog(handle = conversation.handle, onReport = onReport, onDismiss = { showReport = false })
    }

    actionsFor?.let { message ->
        MessageActionsDialog(
            message = message,
            onReact = { emoji ->
                onToggleReaction(message, emoji)
                actionsFor = null
            },
            onMoreReactions = {
                reactingTo = message
                actionsFor = null
            },
            onReply = {
                replyingTo = message
                editing = null
                actionsFor = null
            },
            onEdit = if (canEdit(message)) {
                {
                    editing = message
                    replyingTo = null
                    actionsFor = null
                }
            } else {
                null
            },
            onForward = if (MessageSelection.canForward(message)) {
                {
                    forwarding = listOf(message)
                    actionsFor = null
                }
            } else {
                null
            },
            onCopyText = copyableText(message)?.let { text ->
                {
                    copyMessageText(context, text)
                    actionsFor = null
                }
            },
            onSelectText = copyableText(message)?.let {
                {
                    selectingTextOf = message
                    actionsFor = null
                }
            },
            onSelect = if (MessageSelection.selectable(message)) {
                {
                    selecting = true
                    selected = setOf(message.id)
                    actionsFor = null
                }
            } else {
                null
            },
            pinned = pinned.any { it.messageId == message.id.lowercase() },
            onTogglePin = {
                onTogglePin(message)
                actionsFor = null
            },
            onDeleteForMe = {
                onDeleteForMe(message.id)
                actionsFor = null
            },
            onDeleteForEveryone = {
                pendingDelete = message.id
                actionsFor = null
            },
            onDismiss = { actionsFor = null },
            canPost = !readOnly,
            onReport = if (onReportCommunityMessage != null && !message.fromMe && !message.v3) {
                {
                    reportingMessage = message
                    actionsFor = null
                }
            } else {
                null
            },
        )
    }

    reportingMessage?.let { message ->
        val report = onReportCommunityMessage ?: return@let
        CommunityReportDialog(
            messageText = message.text,
            onSubmit = { category, details, shareText -> report(message, category, details, shareText) },
            onDismiss = { reportingMessage = null },
        )
    }

    pendingDelete?.let { messageId ->
        AlertDialog(
            onDismissRequest = { pendingDelete = null },
            title = { Text("Delete for everyone?") },
            text = {
                val onDeviceKeys = conversation.messages.any { it.id == messageId && it.v3 }
                Text(if (onDeviceKeys) "It's deleted on everyone's devices in this chat. This can't be undone." else "It's deleted from the server for everyone. This can't be undone.")
            },
            confirmButton = {
                TextButton(onClick = {
                    onDelete(messageId)
                    pendingDelete = null
                }) { Text("Delete", color = colors.danger) }
            },
            dismissButton = { TextButton(onClick = { pendingDelete = null }) { Text("Cancel") } },
        )
    }

    forwarding?.let { messages ->
        ForwardDialog(
            messages = messages,
            targets = forwardTargets,
            canSendFiles = canSendFiles,
            onForward = { toIds, onResult -> onForward(messages, toIds, onResult) },
            onDismiss = { forwarding = null },
            onForwarded = {
                forwarding = null
                stopSelecting()
            },
        )
    }

    if (deletingMany) {
        val allMine = MessageSelection.allFromMe(conversation.messages, selected)
        AlertDialog(
            onDismissRequest = { deletingMany = false },
            title = { Text(if (selected.size == 1) "Delete 1 message?" else "Delete ${selected.size} messages?") },
            text = {
                Text(
                    if (allMine) "Delete them from this phone only, or have the server erase them for everyone in the chat (this can't be undone)."
                    else "They're removed from this phone only; the others in the chat keep them. (Only your own messages can be deleted for everyone.)",
                )
            },
            confirmButton = {
                Row {
                    TextButton(onClick = {
                        onDeleteMany(selected, false)
                        deletingMany = false
                        stopSelecting()
                    }) { Text("Delete for me", color = colors.danger) }
                    if (allMine) {
                        TextButton(onClick = {
                            onDeleteMany(selected, true)
                            deletingMany = false
                            stopSelecting()
                        }) { Text("Delete for everyone", color = colors.danger, fontWeight = FontWeight.SemiBold) }
                    }
                }
            },
            dismissButton = { TextButton(onClick = { deletingMany = false }) { Text("Cancel") } },
        )
    }

    viewingPicture?.let { message ->
        val descriptor = message.file ?: return@let
        if (pictures == null) return@let
        PictureViewer(
            descriptor = descriptor,
            pictures = pictures,
            subtitle = "${FileMessage.formatBytes(descriptor.originalSize)} · ${whoDid(message)} · ${TimeFormat.time(message.createdAtMs)}",
            onClose = { viewingPicture = null },
        )
    }

    selectingTextOf?.let { message ->
        // Long-press in here selects part of it, with Android's own Copy.
        AlertDialog(
            onDismissRequest = { selectingTextOf = null },
            title = { Text("Select text") },
            text = {
                SelectionContainer(Modifier.heightIn(max = 420.dp).verticalScroll(rememberScrollState())) {
                    Text(message.text, color = colors.text)
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    copyMessageText(context, message.text)
                    selectingTextOf = null
                }) { Text("Copy all") }
            },
            dismissButton = { TextButton(onClick = { selectingTextOf = null }) { Text("Close") } },
        )
    }

    if (creatingPoll) {
        PollDialog(onCreate = onCreatePoll, onDismiss = { creatingPoll = false })
    }

    reactingTo?.let { message ->
        EmojiPickerDialog(
            onPick = { emoji ->
                reactingTo = null
                onToggleReaction(message, emoji)
            },
            onDismiss = { reactingTo = null },
        )
    }

    versionsOf?.let { message ->
        // The newest first: the text shown now, then each earlier version.
        val versions = (message.earlierVersions + EditVersion(message.text, message.editedAtMs ?: message.createdAtMs)).asReversed()
        AlertDialog(
            onDismissRequest = { versionsOf = null },
            title = { Text("Edit history") },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    versions.forEachIndexed { i, version ->
                        Column {
                            Text(
                                (if (i == 0) "Now · " else "") + TimeFormat.time(version.atMs),
                                color = colors.text3,
                                fontFamily = FontFamily.Monospace,
                                style = MaterialTheme.typography.labelSmall,
                            )
                            Text(version.text, color = if (i == 0) colors.text else colors.text2)
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { versionsOf = null }) { Text("Close") } },
        )
    }

    openLink?.let { url ->
        // Links in messages are untrusted: show the whole address and ask first.
        AlertDialog(
            onDismissRequest = { openLink = null },
            title = { Text("Open this link?") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(url, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall, color = colors.text)
                    Text("It opens in your browser, outside this app.", style = MaterialTheme.typography.bodySmall, color = colors.text2)
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    openLink = null
                    appLock?.expectExternalReturn()
                    runCatching { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
                }) { Text("Open") }
            },
            dismissButton = { TextButton(onClick = { openLink = null }) { Text("Cancel") } },
        )
    }
}

/** The newest pinned message; tapping it jumps there and moves on to the next one. */
@Composable
private fun PinnedBar(pinned: List<PinnedView>, onJump: (String) -> Unit, onUnpin: (String) -> Unit) {
    val colors = RedvaultTheme.colors
    var step by remember { mutableIntStateOf(0) }
    val position = pinned.size - 1 - (step % pinned.size)
    val shown = pinned[position]
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(colors.surface2)
            .clickable {
                onJump(shown.messageId)
                step++
            }
            .padding(start = 16.dp, top = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("📌", modifier = Modifier.padding(end = 10.dp))
        Column(Modifier.weight(1f)) {
            Text(
                if (pinned.size > 1) "Pinned message ${position + 1} of ${pinned.size}" else "Pinned message",
                color = colors.accent,
                fontWeight = FontWeight.SemiBold,
                style = MaterialTheme.typography.labelMedium,
            )
            Text(
                TextFormat.toPlain(shown.preview).ifEmpty { "Message" },
                color = colors.text2,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = MaterialTheme.typography.bodySmall,
            )
        }
        IconButton(onClick = { onUnpin(shown.messageId) }) {
            Icon(painterResource(R.drawable.ic_close), contentDescription = "Unpin", tint = colors.text2, modifier = Modifier.size(18.dp))
        }
    }
}

/** A pin or unpin: a small line in the timeline, not a bubble. */
@Composable
private fun NoticeLine(message: Message, who: String, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    val what = if (message.notice?.kind == NoticeKind.Pin) "pinned “${TextFormat.toPlain(message.text)}”" else "unpinned a message"
    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        Text(
            "📌 $who $what · ${TimeFormat.time(message.createdAtMs)}",
            color = colors.text3,
            style = MaterialTheme.typography.labelMedium,
            textAlign = TextAlign.Center,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .widthIn(max = 320.dp)
                .clip(RoundedCornerShape(12.dp))
                .background(colors.surface2)
                .clickable(onClick = onClick)
                .padding(horizontal = 12.dp, vertical = 4.dp),
        )
    }
}

/** A thread's day header, like the desktop's: a small pill above the day's first message. */
@Composable
internal fun DayHeader(label: String) {
    val colors = RedvaultTheme.colors
    Box(Modifier.fillMaxWidth().padding(top = 10.dp, bottom = 4.dp), contentAlignment = Alignment.Center) {
        Text(
            label,
            color = colors.text2,
            style = MaterialTheme.typography.labelMedium,
            modifier = Modifier
                .clip(RoundedCornerShape(12.dp))
                .background(colors.surface2)
                .semantics { heading() }
                .padding(horizontal = 10.dp, vertical = 4.dp),
        )
    }
}

/** At the top of what's loaded: older messages load when this is reached, or when it's tapped. */
@Composable
internal fun LoadOlderRow(loading: Boolean, onClick: () -> Unit, label: String = "Load older messages") {
    val colors = RedvaultTheme.colors
    Box(Modifier.fillMaxWidth().heightIn(min = 44.dp), contentAlignment = Alignment.Center) {
        if (loading) {
            CircularProgressIndicator(color = colors.accent, strokeWidth = 2.dp, modifier = Modifier.size(24.dp))
        } else {
            TextButton(onClick = onClick) { Text(label, color = colors.accent) }
        }
    }
}

/** Search this chat, like the desktop's: the match count, then older and newer matches. */
@Composable
private fun SearchBar(
    query: String,
    onQueryChange: (String) -> Unit,
    /** The shown match (from 0, oldest first), or -1. */
    position: Int,
    count: Int,
    onOlder: () -> Unit,
    onNewer: () -> Unit,
    /** Only what's loaded is searched: null when there's nothing older (or nothing typed). */
    onSearchOlder: (() -> Unit)?,
    searchingOlder: Boolean,
    onClose: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
    Column(Modifier.fillMaxWidth().background(colors.bg).padding(start = 12.dp, end = 4.dp, top = 4.dp, bottom = 4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = query,
                onValueChange = onQueryChange,
                placeholder = { Text("Search this chat") },
                singleLine = true,
                shape = RoundedCornerShape(22.dp),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                keyboardActions = KeyboardActions(onSearch = { onOlder() }),
                modifier = Modifier.weight(1f).focusRequester(focus),
            )
            IconButton(enabled = count > 1, onClick = onOlder) {
                Icon(painterResource(R.drawable.ui_chevron_down), contentDescription = "Older match", tint = if (count > 1) colors.text else colors.text3, modifier = Modifier.rotate(180f))
            }
            IconButton(enabled = count > 1, onClick = onNewer) {
                Icon(painterResource(R.drawable.ui_chevron_down), contentDescription = "Newer match", tint = if (count > 1) colors.text else colors.text3)
            }
            IconButton(onClick = onClose) {
                Icon(painterResource(R.drawable.ic_close), contentDescription = "Close search", tint = colors.text2)
            }
        }
        if (query.isNotBlank()) {
            Row(Modifier.padding(start = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(
                    if (count == 0) "No matches" else "${(position + 1).coerceAtLeast(1)} of $count",
                    color = colors.text3,
                    style = MaterialTheme.typography.labelMedium,
                    modifier = Modifier.weight(1f).semantics { liveRegion = LiveRegionMode.Polite },
                )
                if (onSearchOlder != null) {
                    TextButton(enabled = !searchingOlder, onClick = onSearchOlder) {
                        Text(if (searchingOlder) "Loading…" else "Search older", color = colors.accent)
                    }
                }
            }
        }
    }
}

/**
 * Forward to one chat or several, with a search box, like the desktop's ForwardDialog. A file is sent
 * again to each chat as a new copy, which needs a plan with file sending.
 */
@Composable
private fun ForwardDialog(
    messages: List<Message>,
    targets: List<Conversation>,
    canSendFiles: Boolean,
    onForward: (toConversationIds: List<String>, onResult: (String?) -> Unit) -> Unit,
    onDismiss: () -> Unit,
    onForwarded: () -> Unit,
) {
    val colors = RedvaultTheme.colors
    var filter by remember { mutableStateOf("") }
    var picked by remember { mutableStateOf(emptyList<String>()) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val hasFile = messages.any { it.kind == MessageKind.File }
    val shown = targets.filter { filter.isBlank() || it.handle.contains(filter.trim(), ignoreCase = true) }
    AlertDialog(
        onDismissRequest = { if (!busy) onDismiss() },
        title = { Text(if (messages.size > 1) "Forward ${messages.size} messages" else "Forward message") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (targets.isNotEmpty()) {
                    OutlinedTextField(
                        value = filter,
                        onValueChange = { filter = it },
                        placeholder = { Text("Search chats") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
                if (shown.isEmpty()) {
                    Text(if (targets.isEmpty()) "You don't have a chat to forward to yet." else "No chats match.", color = colors.text3)
                } else {
                    LazyColumn(Modifier.heightIn(max = 320.dp)) {
                        items(shown, key = { it.id }) { target ->
                            val on = target.id in picked
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clip(RoundedCornerShape(10.dp))
                                    .toggleable(value = on, enabled = !busy, role = Role.Checkbox) {
                                        picked = if (on) picked - target.id else picked + target.id
                                    }
                                    .padding(horizontal = 4.dp, vertical = 6.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(10.dp),
                            ) {
                                Checkbox(checked = on, onCheckedChange = null)
                                if (target.isGroup) GroupAvatar(target.handle, size = 32.dp)
                                else Avatar(avatarUrl = target.avatarUrl, seed = target.handle, size = 32.dp)
                                Text(target.handle, color = colors.text, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                            }
                        }
                    }
                }
                if (hasFile) {
                    Text(
                        if (canSendFiles) "Files are encrypted again on this phone and uploaded once for each chat."
                        else "Forwarding a file means sending it again, which needs Pro.",
                        color = if (canSendFiles) colors.text3 else colors.warn,
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                error?.let { Text(it, color = colors.danger, style = MaterialTheme.typography.bodySmall) }
            }
        },
        confirmButton = {
            TextButton(
                enabled = !busy && picked.isNotEmpty() && (!hasFile || canSendFiles),
                onClick = {
                    busy = true
                    error = null
                    onForward(picked) { result ->
                        busy = false
                        if (result == null) onForwarded() else error = result
                    }
                },
            ) {
                Text(
                    when {
                        busy -> "Forwarding…"
                        picked.size > 1 -> "Forward to ${picked.size} chats"
                        else -> "Forward"
                    },
                    fontWeight = FontWeight.SemiBold,
                )
            }
        },
        dismissButton = { TextButton(enabled = !busy, onClick = onDismiss) { Text("Cancel") } },
    )
}

/** A question and 2-10 options, one answer or several. */
@Composable
private fun PollDialog(onCreate: (String, List<String>, Boolean, (String?) -> Unit) -> Unit, onDismiss: () -> Unit) {
    val colors = RedvaultTheme.colors
    var question by remember { mutableStateOf("") }
    val options = remember { mutableStateListOf("", "") }
    var multiple by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val ready = question.isNotBlank() && options.count { it.isNotBlank() } >= 2
    AlertDialog(
        onDismissRequest = { if (!busy) onDismiss() },
        title = { Text("New poll") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = question,
                    onValueChange = { if (it.length <= RichMessage.MAX_POLL_QUESTION) question = it },
                    placeholder = { Text("Question") },
                    maxLines = 3,
                    modifier = Modifier.fillMaxWidth(),
                )
                options.forEachIndexed { index, option ->
                    OutlinedTextField(
                        value = option,
                        onValueChange = { if (it.length <= RichMessage.MAX_POLL_OPTION) options[index] = it },
                        placeholder = { Text("Option ${index + 1}") },
                        singleLine = true,
                        trailingIcon = if (options.size > 2) {
                            {
                                IconButton(onClick = { options.removeAt(index) }) {
                                    Icon(painterResource(R.drawable.ic_close), contentDescription = "Remove option", modifier = Modifier.size(18.dp))
                                }
                            }
                        } else {
                            null
                        },
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
                if (options.size < RichMessage.MAX_POLL_OPTIONS) {
                    TextButton(onClick = { options += "" }) { Text("Add option") }
                }
                Row(
                    Modifier.fillMaxWidth().clickable { multiple = !multiple },
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Checkbox(checked = multiple, onCheckedChange = { multiple = it })
                    Text("Allow more than one answer", color = colors.text)
                }
                error?.let { Text(it, color = colors.danger, style = MaterialTheme.typography.bodySmall) }
            }
        },
        confirmButton = {
            TextButton(enabled = ready && !busy, onClick = {
                busy = true
                error = null
                onCreate(question, options.toList(), multiple) { result ->
                    busy = false
                    if (result == null) onDismiss() else error = result
                }
            }) { Text(if (busy) "Sending…" else "Send poll") }
        },
        dismissButton = { TextButton(enabled = !busy, onClick = onDismiss) { Text("Cancel") } },
    )
}

/** A group's menu next to Info: search the chat, or tick messages to forward or delete. */
@Composable
private fun GroupThreadMenu(onSearch: () -> Unit, onSelect: () -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        IconButton(onClick = { open = true }) {
            Icon(painterResource(R.drawable.ic_more_vert), contentDescription = "More options")
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            DropdownMenuItem(text = { Text("Search") }, onClick = {
                open = false
                onSearch()
            })
            DropdownMenuItem(text = { Text("Select messages") }, onClick = {
                open = false
                onSelect()
            })
        }
    }
}

@Composable
private fun ThreadMenu(
    blocked: Boolean,
    onSafetyNumber: () -> Unit,
    onReport: () -> Unit,
    onToggleBlock: () -> Unit,
    onChatDetails: (() -> Unit)? = null,
    onSharedMedia: (() -> Unit)? = null,
    onSearch: () -> Unit = {},
    onSelect: () -> Unit = {},
) {
    var open by remember { mutableStateOf(false) }
    Box {
        IconButton(onClick = { open = true }) {
            Icon(painterResource(R.drawable.ic_more_vert), contentDescription = "More options")
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            onChatDetails?.let { details ->
                DropdownMenuItem(text = { Text("Chat details") }, onClick = {
                    open = false
                    details()
                })
            }
            onSharedMedia?.let { media ->
                DropdownMenuItem(text = { Text("Shared media and files") }, onClick = {
                    open = false
                    media()
                })
            }
            DropdownMenuItem(text = { Text("Search") }, onClick = {
                open = false
                onSearch()
            })
            DropdownMenuItem(text = { Text("Select messages") }, onClick = {
                open = false
                onSelect()
            })
            DropdownMenuItem(text = { Text("Safety number") }, onClick = {
                open = false
                onSafetyNumber()
            })
            DropdownMenuItem(text = { Text(if (blocked) "Unblock" else "Block") }, onClick = {
                open = false
                onToggleBlock()
            })
            DropdownMenuItem(text = { Text("Report") }, onClick = {
                open = false
                onReport()
            })
        }
    }
}

/** Port of the web profile's report flow: only the written reason goes to the moderators, never message content. */
@Composable
private fun ReportDialog(handle: String, onReport: suspend (String) -> Unit, onDismiss: () -> Unit) {
    val scope = rememberCoroutineScope()
    var reason by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var sent by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (sent) "Report sent" else "Report $handle") },
        text = {
            if (sent) {
                Text("Thanks. The moderators will review it.")
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        "Tell the moderators what happened. Messages are end-to-end encrypted, so they can't read your chat; only what you write here is sent.",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    OutlinedTextField(
                        value = reason,
                        onValueChange = { if (it.length <= 1000) reason = it },
                        placeholder = { Text("Reason") },
                        minLines = 3,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    error?.let { Text(it, color = RedvaultTheme.colors.danger, style = MaterialTheme.typography.bodySmall) }
                }
            }
        },
        confirmButton = {
            if (sent) {
                TextButton(onClick = onDismiss) { Text("Done") }
            } else {
                TextButton(enabled = reason.isNotBlank() && !busy, onClick = {
                    busy = true
                    error = null
                    scope.launch {
                        try {
                            onReport(reason)
                            sent = true
                        } catch (e: Exception) {
                            error = describeError(e, "Couldn't send the report. Try again.")
                        } finally {
                            busy = false
                        }
                    }
                }) { Text(if (busy) "Sending..." else "Send report", color = RedvaultTheme.colors.danger) }
            }
        },
        dismissButton = { if (!sent) TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** Long-press menu: react, reply, edit, forward, pin, delete for me, and delete for everyone on your own messages. */
@Composable
private fun MessageActionsDialog(
    message: Message,
    onReact: (String) -> Unit,
    /** "+" after the quick reactions: react with any emoji. */
    onMoreReactions: () -> Unit,
    onReply: () -> Unit,
    /** Null when this message can't be edited (someone else's, not text, or over a day old). */
    onEdit: (() -> Unit)?,
    onForward: (() -> Unit)?,
    pinned: Boolean,
    onTogglePin: () -> Unit,
    onDeleteForMe: () -> Unit,
    onDeleteForEveryone: () -> Unit,
    onDismiss: () -> Unit,
    /** False in a channel this user only reads: reacting, replying and pinning would all be posts. */
    canPost: Boolean = true,
    /** Communities: report this message (null elsewhere, and for your own). */
    onReport: (() -> Unit)? = null,
    /** Text messages only: copy all of it, or open it to select part of it. */
    onCopyText: (() -> Unit)? = null,
    onSelectText: (() -> Unit)? = null,
    /** Start ticking messages, this one first (to forward or delete several). */
    onSelect: (() -> Unit)? = null,
) {
    val colors = RedvaultTheme.colors
    AlertDialog(
        onDismissRequest = onDismiss,
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                if (canPost) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                    RichMessage.REACTION_EMOJI.forEach { emoji ->
                        val mine = message.reactions.any { it.emoji == emoji && it.mine }
                        Text(
                            emoji,
                            style = MaterialTheme.typography.headlineSmall,
                            modifier = Modifier
                                .background(if (mine) colors.accentSoft else Color.Transparent, RoundedCornerShape(50))
                                .clickable { onReact(emoji) }
                                .padding(6.dp),
                        )
                    }
                    // Any other emoji, from the full picker.
                    Text(
                        "+",
                        color = colors.text2,
                        style = MaterialTheme.typography.headlineSmall,
                        modifier = Modifier
                            .clip(RoundedCornerShape(50))
                            .background(colors.surface2)
                            .clickable(onClickLabel = "More reactions", onClick = onMoreReactions)
                            .padding(horizontal = 10.dp, vertical = 2.dp),
                    )
                }
                // Device-key messages have no replies, pins or "delete for me" yet, as on the web.
                if (canPost && !message.v3) TextButton(onClick = onReply) { Text("Reply") }
                onEdit?.let { TextButton(onClick = it) { Text("Edit") } }
                onCopyText?.let { TextButton(onClick = it) { Text("Copy text") } }
                onSelectText?.let { TextButton(onClick = it) { Text("Select text") } }
                onForward?.let { TextButton(onClick = it) { Text("Forward") } }
                if (canPost && !message.v3) TextButton(onClick = onTogglePin) { Text(if (pinned) "Unpin" else "Pin") }
                onSelect?.let { TextButton(onClick = it) { Text("Select") } }
                if (!message.v3) TextButton(onClick = onDeleteForMe) { Text("Delete for me") }
                if (message.fromMe) TextButton(onClick = onDeleteForEveryone) { Text("Delete for everyone", color = colors.danger) }
                onReport?.let { TextButton(onClick = it) { Text("Report", color = colors.danger) } }
            }
        },
        confirmButton = {},
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** Copy text: kept out of Android's clipboard preview, like the desktop's (which keeps it out of clipboard history). */
private fun copyMessageText(context: android.content.Context, text: String) {
    SecretClipboard.copyPrivate(context, text)
    android.widget.Toast.makeText(context, "Copied", android.widget.Toast.LENGTH_SHORT).show()
}

@Composable
private fun VerifiedChip(verified: Boolean, onClick: () -> Unit) {
    val colors = RedvaultTheme.colors
    Text(
        if (verified) "Verified" else "Not verified",
        color = if (verified) colors.accent else colors.warn,
        style = MaterialTheme.typography.labelMedium,
        modifier = Modifier
            .padding(end = 12.dp)
            .background(if (verified) colors.accentSoft else colors.warnSoft, RoundedCornerShape(50))
            .clickable(onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 4.dp),
    )
}

/** In place of the composer in a channel this user only reads. */
@Composable
private fun ReadOnlyBar(modifier: Modifier = Modifier) {
    val colors = RedvaultTheme.colors
    Row(
        modifier.fillMaxWidth().background(colors.surface).padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Icon(painterResource(R.drawable.ui_lock), contentDescription = null, tint = colors.text3, modifier = Modifier.size(14.dp))
        Text("Only the creator and staff post in this channel.", color = colors.text3, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun Banner(text: String, action: String? = null, onClick: (() -> Unit)? = null, positive: Boolean = false) {
    val colors = RedvaultTheme.colors
    val tint = if (positive) colors.accent else colors.warn
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(if (positive) colors.accentSoft else colors.warnSoft)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(text, color = tint, style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(1f))
        if (action != null) {
            Text(action, color = tint, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(start = 12.dp))
        }
    }
}

@Composable
private fun DisappearingRow(current: Disappearing, onChange: (Disappearing) -> Unit) {
    val colors = RedvaultTheme.colors
    var open by remember { mutableStateOf(false) }
    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("Disappearing messages", style = MaterialTheme.typography.labelMedium, color = colors.text3, modifier = Modifier.weight(1f))
        Box {
            Text(
                "${current.label} ▾",
                style = MaterialTheme.typography.labelMedium,
                color = colors.text,
                modifier = Modifier
                    .background(colors.surface2, RoundedCornerShape(50))
                    .clickable { open = true }
                    .padding(horizontal = 12.dp, vertical = 6.dp),
            )
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                Disappearing.entries.forEach { option ->
                    DropdownMenuItem(
                        text = { Text(option.label, fontWeight = if (option == current) FontWeight.SemiBold else FontWeight.Normal) },
                        onClick = {
                            open = false
                            if (option != current) onChange(option)
                        },
                    )
                }
            }
        }
    }
}

/** A compact status from real events only, never a decorative checkmark: an icon and a word. */
private fun compactStatus(message: Message): Pair<Int, String> = when {
    // Waiting in the Tor outbox: not sent, so never "Sent" or "Delivered".
    message.queueHeld -> R.drawable.ic_alert to "Not sent"
    message.queued -> R.drawable.ui_clock to "Queued"
    message.security?.verification == VerificationStatus.Failed -> R.drawable.ic_alert to "Server's copy differs"
    message.seen -> R.drawable.ic_check_check to "Read"
    message.delivered -> R.drawable.ic_check_check to "Delivered"
    message.security?.verification == VerificationStatus.Verified -> R.drawable.ic_shield_check to "Sent, server's copy matches"
    message.v3 -> R.drawable.ic_shield_check to "Device keys"
    else -> R.drawable.ic_check to "Sent"
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun MessageBubble(
    message: Message,
    onRequestVerification: () -> Unit,
    onOpenDetails: () -> Unit,
    onDownloadFile: DownloadFile,
    onLongPress: () -> Unit,
    onToggleReaction: (String) -> Unit,
    onRetryQueued: () -> Unit = {},
    onDiscardQueued: () -> Unit = {},
    senderName: String? = null,
    /** Handles that can be mentioned here (a group's members). */
    mentions: List<String> = emptyList(),
    selfHandle: String? = null,
    onOpenLink: (String) -> Unit = {},
    onShowVersions: () -> Unit = {},
    onJumpToReply: () -> Unit = {},
    onVote: (List<Int>) -> Unit = {},
    /** Only for your own polls. */
    onEndPoll: (() -> Unit)? = null,
    highlighted: Boolean = false,
    /** "Forwarded from <channel>": opens that channel. */
    onOpenForwardedChannel: (() -> Unit)? = null,
    onChannelPostShown: ((postId: String) -> Unit)? = null,
    /** Pictures show in the bubble; null shows them as files. */
    pictures: ChatPictures? = null,
    onOpenPicture: () -> Unit = {},
    /** Selecting messages: whether this one is ticked (null when not selecting). */
    selected: Boolean? = null,
) {
    val colors = RedvaultTheme.colors
    var expanded by rememberSaveable(message.id) { mutableStateOf(false) }
    val sent = message.fromMe
    val background = if (sent) colors.accent else colors.surface
    val foreground = if (sent) colors.accentContrast else colors.text
    val meta = if (sent) colors.accentContrast.copy(alpha = 0.75f) else colors.text3
    val shape = if (sent) RoundedCornerShape(15.dp, 15.dp, 4.dp, 15.dp) else RoundedCornerShape(15.dp, 15.dp, 15.dp, 4.dp)

    Row(
        Modifier
            .fillMaxWidth()
            .then(if (selected == true) Modifier.background(colors.accentSoft, RoundedCornerShape(12.dp)) else Modifier),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (selected != null) SelectMark(selected, enabled = MessageSelection.selectable(message))
        Box(Modifier.weight(1f), contentAlignment = if (sent) Alignment.CenterEnd else Alignment.CenterStart) {
            Column(
                modifier = Modifier
                    .widthIn(max = 300.dp)
                    .background(background, shape)
                    .then(if (highlighted) Modifier.border(2.dp, if (sent) colors.accentContrast else colors.accent, shape) else Modifier)
                    .combinedClickable(
                        onClick = {
                            if (!sent || message.queued) return@combinedClickable
                            expanded = !expanded
                            if (expanded && message.security == null) onRequestVerification()
                        },
                        onLongClick = onLongPress,
                    )
                    .padding(horizontal = 12.dp, vertical = 8.dp),
            ) {
                senderName?.let { Text(it, color = colors.accent, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.labelMedium) }
                // No signature where their apps now sign: an older app of theirs, or someone else made it.
                if (message.unverifiedSender) {
                    Text("⚠ Sender not verified", color = colors.danger, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.labelSmall)
                }
                val channelPost = message.channelPost
                if (channelPost != null) {
                    // A forwarded channel post counts as a view of the original (each phone counts a post once).
                    LaunchedEffect(channelPost.postId) { onChannelPostShown?.invoke(channelPost.postId) }
                    Text(
                        "↪ Forwarded from ${channelPost.channelName}",
                        color = meta,
                        fontStyle = androidx.compose.ui.text.font.FontStyle.Italic,
                        textDecoration = if (onOpenForwardedChannel != null) androidx.compose.ui.text.style.TextDecoration.Underline else null,
                        style = MaterialTheme.typography.labelSmall,
                        modifier = if (onOpenForwardedChannel != null) Modifier.clickable(role = Role.Button, onClick = onOpenForwardedChannel) else Modifier,
                    )
                } else if (message.forwarded) {
                    Text("↪ Forwarded", color = meta, fontStyle = androidx.compose.ui.text.font.FontStyle.Italic, style = MaterialTheme.typography.labelSmall)
                }
                message.storyReply?.let { story ->
                    Column(
                        Modifier
                            .padding(bottom = 6.dp)
                            .clip(RoundedCornerShape(6.dp))
                            .background(foreground.copy(alpha = 0.12f))
                            .padding(horizontal = 8.dp, vertical = 4.dp),
                    ) {
                        Text(if (message.fromMe) "Your reply to their story" else "Reply to your story", color = foreground, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.labelSmall)
                        Text(TextFormat.toPlain(story.preview).ifEmpty { "Story" }, color = foreground.copy(alpha = 0.8f), maxLines = 2, style = MaterialTheme.typography.bodySmall)
                    }
                }
                message.replyTo?.let { reply ->
                    Column(
                        Modifier
                            .padding(bottom = 6.dp)
                            .clip(RoundedCornerShape(6.dp))
                            .background(foreground.copy(alpha = 0.12f))
                            .clickable(onClick = onJumpToReply)
                            .padding(horizontal = 8.dp, vertical = 4.dp),
                    ) {
                        Text(if (reply.fromMe) "You" else "Them", color = foreground, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.labelSmall)
                        Text(
                            TextFormat.toPlain(reply.preview).ifEmpty { "Message" },
                            color = foreground.copy(alpha = 0.8f),
                            maxLines = 2,
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                }
                when (message.kind) {
                    MessageKind.Text -> {
                        val poll = message.poll
                        if (message.deleted) {
                            Text(message.text, color = foreground.copy(alpha = 0.7f), fontStyle = androidx.compose.ui.text.font.FontStyle.Italic)
                        } else if (poll != null) {
                            PollCard(poll, foreground, meta, onVote, onEndPoll)
                        } else {
                            FormattedText(
                                text = message.text,
                                color = foreground,
                                onOpenLink = onOpenLink,
                                mentions = mentions,
                                selfHandle = selfHandle,
                                linkColor = if (sent) foreground else colors.accent,
                            )
                        }
                    }
                    // Queued: encrypted in the outbox, not uploaded yet, so nothing to open.
                    MessageKind.File -> when {
                        message.queued -> Text("📎 ${message.text}", color = foreground)
                        pictures != null && PictureRules.isInlineImage(message) ->
                            PictureBubbleContent(message, pictures, meta, onOpen = onOpenPicture, onLongPress = onLongPress)
                        else -> FileBubbleContent(message, foreground, meta, onDownloadFile)
                    }
                    MessageKind.Undecryptable -> Text(message.text, color = foreground.copy(alpha = 0.7f))
                }
                Row(
                    modifier = Modifier.padding(top = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    if (message.editedAtMs != null) {
                        Text(
                            "edited",
                            color = meta,
                            style = MaterialTheme.typography.labelSmall,
                            modifier = Modifier.clickable(onClickLabel = "Show earlier versions", onClick = onShowVersions),
                        )
                    }
                    Icon(painterResource(R.drawable.ic_lock), contentDescription = "End-to-end encrypted", tint = meta, modifier = Modifier.size(11.dp))
                    Text(TimeFormat.time(message.createdAtMs), color = meta, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.labelSmall)
                    if (sent) {
                        val (icon, label) = compactStatus(message)
                        Icon(painterResource(icon), contentDescription = null, tint = meta, modifier = Modifier.size(12.dp))
                        Text(label, color = meta, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.labelSmall)
                    }
                }
                if (message.reactions.isNotEmpty()) {
                    Row(Modifier.padding(top = 6.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        message.reactions.forEach { r ->
                            Text(
                                r.emoji + if (r.count > 1) " ${r.count}" else "",
                                color = foreground,
                                fontWeight = if (r.mine) FontWeight.Bold else FontWeight.Normal,
                                modifier = Modifier
                                    .background(foreground.copy(alpha = if (r.mine) 0.22f else 0.1f), RoundedCornerShape(50))
                                    .clickable { onToggleReaction(r.emoji) }
                                    .padding(horizontal = 8.dp, vertical = 2.dp),
                            )
                        }
                    }
                }
                if (message.queued) QueuedRow(message, meta, foreground, onRetryQueued, onDiscardQueued)
                if (expanded && sent) SecurityPanel(message, meta, foreground, onOpenDetails)
            }
        }
    }
}

/** Selecting: a round tick at the start of each row (empty on rows that can't be ticked). */
@Composable
private fun SelectMark(selected: Boolean, enabled: Boolean) {
    val colors = RedvaultTheme.colors
    Box(
        Modifier
            .padding(start = 4.dp, end = 8.dp)
            .size(22.dp)
            .clip(CircleShape)
            .then(
                when {
                    !enabled -> Modifier
                    selected -> Modifier.background(colors.accent)
                    else -> Modifier.border(2.dp, colors.text3, CircleShape)
                },
            )
            .semantics { stateDescription = if (selected) "Ticked" else "Not ticked" },
        contentAlignment = Alignment.Center,
    ) {
        if (enabled && selected) Icon(painterResource(R.drawable.ic_check), contentDescription = null, tint = colors.accentContrast, modifier = Modifier.size(14.dp))
    }
}

/** Under a queued message: why it's waiting, and Retry (if the server refused it) or Discard. */
@Composable
private fun QueuedRow(message: Message, meta: Color, foreground: Color, onRetry: () -> Unit, onDiscard: () -> Unit) {
    Column(Modifier.padding(top = 4.dp)) {
        val reason = message.queueError ?: "Waiting to send over Tor"
        Text(reason, color = meta, style = MaterialTheme.typography.labelSmall)
        Row(horizontalArrangement = Arrangement.spacedBy(14.dp), modifier = Modifier.padding(top = 2.dp)) {
            if (message.queueHeld) {
                Text(
                    "Retry",
                    color = foreground,
                    fontWeight = FontWeight.SemiBold,
                    style = MaterialTheme.typography.labelMedium,
                    modifier = Modifier.clickable(onClickLabel = "Send it again", onClick = onRetry),
                )
            }
            Text(
                "Discard",
                color = foreground,
                fontWeight = FontWeight.SemiBold,
                style = MaterialTheme.typography.labelMedium,
                modifier = Modifier.clickable(onClickLabel = "Delete it without sending", onClick = onDiscard),
            )
        }
    }
}

/** A poll: tap an option to vote (again to take it back) until its creator ends it. */
@Composable
private fun PollCard(poll: PollView, foreground: Color, meta: Color, onVote: (List<Int>) -> Unit, onEndPoll: (() -> Unit)?) {
    Column(Modifier.widthIn(min = 220.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(
            when {
                poll.closed -> "Poll · ended"
                poll.multiple -> "Poll · pick any"
                else -> "Poll · pick one"
            },
            color = meta,
            style = MaterialTheme.typography.labelSmall,
        )
        Text(poll.question, color = foreground, fontWeight = FontWeight.SemiBold)
        poll.options.forEachIndexed { index, label ->
            val mine = index in poll.mine
            val count = poll.counts.getOrElse(index) { 0 }
            val share = if (poll.voters > 0) count.toFloat() / poll.voters else 0f
            Box(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(8.dp))
                    .background(foreground.copy(alpha = 0.08f))
                    .clickable(enabled = !poll.closed) {
                        onVote(
                            when {
                                mine -> poll.mine - index
                                poll.multiple -> poll.mine + index
                                else -> listOf(index)
                            },
                        )
                    },
            ) {
                Box(Modifier.matchParentSize()) {
                    Box(Modifier.fillMaxHeight().fillMaxWidth(share).background(foreground.copy(alpha = 0.16f)))
                }
                Row(
                    Modifier.padding(horizontal = 10.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(if (poll.multiple) (if (mine) "☑" else "☐") else (if (mine) "◉" else "○"), color = foreground)
                    Text(label, color = foreground, fontWeight = if (mine) FontWeight.SemiBold else FontWeight.Normal, modifier = Modifier.weight(1f))
                    Text(count.toString(), color = meta, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.labelMedium)
                }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (poll.voters == 1) "1 person voted" else "${poll.voters} people voted",
                color = meta,
                style = MaterialTheme.typography.labelSmall,
                modifier = Modifier.weight(1f),
            )
            if (onEndPoll != null && !poll.closed) {
                Text(
                    "End poll",
                    color = foreground,
                    fontWeight = FontWeight.SemiBold,
                    style = MaterialTheme.typography.labelMedium,
                    modifier = Modifier.clip(RoundedCornerShape(6.dp)).clickable(onClick = onEndPoll).padding(horizontal = 6.dp, vertical = 2.dp),
                )
            }
        }
    }
}

@Composable
private fun SecurityPanel(message: Message, muted: Color, foreground: Color, onOpenDetails: () -> Unit) {
    val verification = message.security?.verification
    if (message.v3) {
        // Per-device keys: there is no public transaction record, delivery or read receipt for these yet, so nothing
        // here pretends otherwise.
        Column(Modifier.padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text("MESSAGE SECURITY", color = muted, style = MaterialTheme.typography.labelSmall)
            SecurityRow(if (message.fromMe) "Encrypted on this phone, for each device" else "Decrypted on this phone", SecurityMark.Done, foreground)
            if (message.fromMe) SecurityRow("Relayed as ciphertext", if (message.queued) SecurityMark.Failed else SecurityMark.Done, foreground)
            Text(
                "Sent with per-device keys (preview). The server kept only ciphertext until each device fetched it. No public record, delivered or read marks yet.",
                color = muted,
                style = MaterialTheme.typography.labelSmall,
            )
        }
        return
    }
    Column(Modifier.padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text("MESSAGE SECURITY", color = muted, style = MaterialTheme.typography.labelSmall)
        SecurityRow("Encrypted locally", SecurityMark.Done, foreground)
        SecurityRow("Relayed as ciphertext", SecurityMark.Done, foreground)
        SecurityRow(
            "Server's copy matches",
            when (verification) {
                VerificationStatus.Verified -> SecurityMark.Done
                VerificationStatus.Failed -> SecurityMark.Failed
                VerificationStatus.Unavailable -> SecurityMark.Unavailable
                null -> SecurityMark.Pending
            },
            foreground,
        )
        SecurityRow("Delivered", if (message.delivered) SecurityMark.Done else SecurityMark.Pending, foreground)
        SecurityRow("Read", if (message.seen) SecurityMark.Done else SecurityMark.Pending, foreground)
        message.security?.let { security ->
            if (security.verification == VerificationStatus.Failed) {
                Text(
                    "The server's record of this message doesn't match what this phone sent.",
                    color = foreground,
                    fontWeight = FontWeight.SemiBold,
                    style = MaterialTheme.typography.labelSmall,
                )
            }
            if (security.publicTransactionId != null) {
                Text(
                    "View verification details",
                    color = foreground,
                    fontWeight = FontWeight.SemiBold,
                    style = MaterialTheme.typography.labelMedium,
                    modifier = Modifier.padding(top = 4.dp).clickable(onClick = onOpenDetails),
                )
            }
        }
    }
}

private enum class SecurityMark { Done, Failed, Unavailable, Pending }

@Composable
private fun SecurityRow(label: String, mark: SecurityMark, color: Color) {
    Row(Modifier.widthIn(min = 200.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = color, style = MaterialTheme.typography.labelMedium, modifier = Modifier.weight(1f))
        when (mark) {
            SecurityMark.Done -> Icon(painterResource(R.drawable.ic_check), contentDescription = "Yes", tint = color, modifier = Modifier.size(14.dp))
            SecurityMark.Failed -> Icon(painterResource(R.drawable.ic_close), contentDescription = "Failed", tint = color, modifier = Modifier.size(14.dp))
            SecurityMark.Unavailable -> Text("–", color = color, style = MaterialTheme.typography.labelMedium)
            SecurityMark.Pending -> Text("…", color = color, style = MaterialTheme.typography.labelMedium)
        }
    }
}

/** A pasted picture is held in memory until it's sent; past this, it has to be attached as a file. */
private const val MAX_PASTED_BYTES = 20L * 1024 * 1024

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun Composer(
    onSend: (String, (Boolean) -> Unit) -> Unit,
    replyingTo: Message?,
    otherHandle: String,
    onCancelReply: () -> Unit,
    /** Set while editing one of your messages: Send then sends the new text. */
    editing: Message?,
    onCancelEdit: () -> Unit,
    /** Handles "@" suggests (a group's other members). */
    mentionCandidates: List<String>,
    onStartPoll: () -> Unit,
    onSendFiles: (List<PendingFile>, (Int, FileSendStage) -> Unit, (Int, String?) -> Unit) -> Unit,
    onSendVoice: (File, (FileSendStage) -> Unit, (String?) -> Unit) -> Unit,
    canSendFiles: Boolean,
    modifier: Modifier = Modifier,
    onTyping: () -> Unit = {},
    /** The chat's unsent text from last time; [onDraftChange] gets the box's text back when the chat closes. */
    initialDraft: String = "",
    onDraftChange: (String) -> Unit = {},
    /** Shared from another app: files wait in the box to be sent, text goes after what's typed. */
    incomingShare: SharedItems? = null,
    onShareTaken: () -> Unit = {},
) {
    val colors = RedvaultTheme.colors
    val context = LocalContext.current
    // Plain remember: a draft shouldn't be written into saved instance state. The session keeps it
    // (in memory only) once the chat closes, for the chat list's "Draft:".
    val draft = remember { TextFieldState(initialDraft) }
    var sending by remember { mutableStateOf(false) }
    val text = draft.text.toString()
    val canSend = text.isNotBlank() && !sending
    var attachMenu by remember { mutableStateOf(false) }
    var pickingEmoji by remember { mutableStateOf(false) }

    // Editing starts from the message's current text; leaving it brings back what was typed before.
    var draftBeforeEdit by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(editing?.id) {
        val editText = editing?.text
        if (editText != null) {
            if (draftBeforeEdit == null) draftBeforeEdit = draft.text.toString()
            draft.setTextAndPlaceCursorAtEnd(editText)
        } else if (draftBeforeEdit != null) {
            draft.setTextAndPlaceCursorAtEnd(draftBeforeEdit.orEmpty())
            draftBeforeEdit = null
        }
    }
    val saveDraft by rememberUpdatedState(onDraftChange)
    DisposableEffect(Unit) {
        onDispose { saveDraft(draftBeforeEdit ?: draft.text.toString()) }
    }
    // Typing and pasting only, never text set here: tells the others (while typing indicators are on).
    val typing by rememberUpdatedState(onTyping)
    val typingSignal = remember {
        object : InputTransformation {
            override fun TextFieldBuffer.transformInput() {
                if (length > 0) typing()
            }
        }
    }

    // "@partial" right before the cursor: suggest the members it could be.
    val beforeCursor = text.substring(0, draft.selection.end.coerceIn(0, text.length))
    val partial = if (mentionCandidates.isEmpty()) null else MENTION_PARTIAL.find(beforeCursor)?.groupValues?.get(1)
    val suggestions = if (partial == null) emptyList() else mentionCandidates.filter { it.startsWith(partial, ignoreCase = true) }.take(5)

    // Attachment flow: pick one file or several -> preview (names/sizes, local only) -> Encrypting ->
    // Uploading -> Sending -> Sent, one file after another.
    val pendingFiles = remember { mutableStateListOf<PendingFile>() }
    var fileStage by remember { mutableStateOf<FileSendStage?>(null) }
    var sendingIndex by remember { mutableIntStateOf(0) }
    var sendingCount by remember { mutableIntStateOf(0) }
    var fileSent by remember { mutableStateOf(false) }
    var fileError by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    fun addFiles(files: List<PendingFile>) {
        if (files.isEmpty()) return
        fileSent = false
        if (!canSendFiles) {
            fileError = PRO_REQUIRED_MESSAGE
            return
        }
        pendingFiles += files
        fileError = null
    }
    fun sendFiles() {
        val batch = pendingFiles.toList()
        if (batch.isEmpty()) return
        fileError = null
        fileSent = false
        sendingCount = batch.size
        sendingIndex = 0
        fileStage = FileSendStage.Encrypting
        onSendFiles(batch, { index, stage ->
            sendingIndex = index
            fileStage = stage
        }) { sent, error ->
            fileStage = null
            val gone = batch.take(sent)
            pendingFiles.removeAll { pending -> gone.any { it === pending } }
            if (error != null) {
                fileError = error
            } else {
                fileSent = true
                scope.launch {
                    kotlinx.coroutines.delay(2_000)
                    fileSent = false
                }
            }
        }
    }
    // Voice messages: tap the mic to record, then Send or Cancel. Pro-only, like files.
    val recorder = remember { VoiceRecorder(context) }
    var recording by remember { mutableStateOf(false) }
    var recordedMs by remember { mutableStateOf(0L) }
    DisposableEffect(Unit) { onDispose { recorder.cancel() } }
    LaunchedEffect(recording) {
        while (recording) {
            recordedMs = recorder.elapsedMs()
            kotlinx.coroutines.delay(250)
        }
    }
    fun sendRecording() {
        recording = false
        val file = recorder.stop() ?: return
        fileError = null
        fileSent = false
        sendingCount = 0
        fileStage = FileSendStage.Encrypting
        onSendVoice(file, { fileStage = it }) { error ->
            fileStage = null
            if (error == null) {
                fileSent = true
                scope.launch {
                    kotlinx.coroutines.delay(2_000)
                    fileSent = false
                }
            } else {
                fileError = error
            }
        }
    }
    fun startRecording() {
        try {
            recorder.start(onLimitReached = { sendRecording() })
            recording = true
            fileError = null
        } catch (e: Exception) {
            fileError = "Couldn't start recording."
        }
    }
    val micPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) startRecording() else fileError = "Allow microphone access to send voice messages."
    }

    val appLock = LocalAppLock.current
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) scope.launch { addFiles(withContext(Dispatchers.IO) { uris.map { describeShared(context, it) } }) }
    }

    // A picture pasted into the box, or put there by the keyboard: read into memory now (the permission
    // to read it may not last), then it waits with any other files.
    fun pastePicture(uri: Uri, mimeType: String) {
        if (!canSendFiles) {
            fileSent = false
            fileError = PRO_REQUIRED_MESSAGE
            return
        }
        scope.launch {
            val pasted = withContext(Dispatchers.IO) { readPastedPicture(context, uri, mimeType) }
            when {
                pasted == null -> fileError = "Couldn't paste that picture. Attach it as a file instead."
                pasted.bytes == null -> fileError = "That picture is too large to paste. Attach it as a file instead."
                else -> addFiles(listOf(pasted))
            }
        }
    }
    val paste by rememberUpdatedState<(Uri, String) -> Unit> { uri, type -> pastePicture(uri, type) }
    val pictureReceiver = remember {
        ReceiveContentListener { content ->
            if (!content.hasMediaType(MediaType.Image)) return@ReceiveContentListener content
            val description = content.clipEntry.clipData.description
            content.consume { item ->
                val uri = item.uri ?: return@consume false
                val type = runCatching { context.contentResolver.getType(uri) }.getOrNull()
                    ?: (0 until description.mimeTypeCount).map(description::getMimeType).firstOrNull { it.startsWith("image/") }
                if (type == null || !type.startsWith("image/")) return@consume false
                paste(uri, type)
                true
            }
        }
    }

    // Shared from another app into this chat.
    LaunchedEffect(incomingShare) {
        val share = incomingShare ?: return@LaunchedEffect
        if (share.text.isNotBlank()) {
            val current = draft.text.toString()
            draft.setTextAndPlaceCursorAtEnd(if (current.isBlank()) share.text else current.trimEnd() + "\n" + share.text)
        }
        if (share.uris.isNotEmpty()) addFiles(withContext(Dispatchers.IO) { share.uris.map { describeShared(context, it) } })
        onShareTaken()
    }

    Column(modifier.fillMaxWidth().background(colors.bg)) {
        replyingTo?.let { target ->
            Row(
                Modifier.fillMaxWidth().background(colors.surface2).padding(horizontal = 16.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text(
                        "Replying to " + if (target.fromMe) "yourself" else otherHandle,
                        color = colors.accent,
                        fontWeight = FontWeight.SemiBold,
                        style = MaterialTheme.typography.labelMedium,
                    )
                    Text(
                        if (target.kind == MessageKind.File) "📎 ${target.text}" else TextFormat.toPlain(target.text),
                        color = colors.text2,
                        maxLines = 1,
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                IconButton(onClick = onCancelReply) {
                    Icon(painterResource(R.drawable.ic_close), contentDescription = "Cancel reply", tint = colors.text2, modifier = Modifier.size(20.dp))
                }
            }
        }
        editing?.let { target ->
            Row(
                Modifier.fillMaxWidth().background(colors.surface2).padding(horizontal = 16.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text("Editing message", color = colors.accent, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.labelMedium)
                    Text(TextFormat.toPlain(target.text), color = colors.text2, maxLines = 1, style = MaterialTheme.typography.bodySmall)
                }
                IconButton(onClick = onCancelEdit) {
                    Icon(painterResource(R.drawable.ic_close), contentDescription = "Cancel editing", tint = colors.text2, modifier = Modifier.size(20.dp))
                }
            }
        }
        if (suggestions.isNotEmpty() && partial != null) {
            Row(
                Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(start = 12.dp, end = 12.dp, top = 6.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                suggestions.forEach { handle ->
                    Text(
                        "@$handle",
                        color = colors.accent,
                        fontWeight = FontWeight.SemiBold,
                        style = MaterialTheme.typography.labelLarge,
                        modifier = Modifier
                            .clip(RoundedCornerShape(50))
                            .background(colors.accentSoft)
                            .clickable {
                                // Replace "@partial" before the cursor with the full handle and a space.
                                val start = beforeCursor.length - partial.length - 1
                                val inserted = "@$handle "
                                draft.edit {
                                    replace(start, beforeCursor.length, inserted)
                                    selection = TextRange(start + inserted.length)
                                }
                            }
                            .padding(horizontal = 12.dp, vertical = 6.dp),
                    )
                }
            }
        }
        AttachmentBar(
            pending = pendingFiles,
            stage = fileStage,
            progress = if (fileStage != null && sendingCount > 0) (sendingIndex + 1) to sendingCount else null,
            sent = fileSent,
            error = fileError,
            onSend = ::sendFiles,
            onCancel = {
                pendingFiles.clear()
                fileError = null
            },
            onRemove = { file -> pendingFiles.removeAll { it === file } },
        )
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Box {
                IconButton(enabled = fileStage == null && editing == null, onClick = { attachMenu = true }) {
                    Icon(painterResource(R.drawable.ic_attach), contentDescription = "Attach files or a poll", tint = colors.text2)
                }
                DropdownMenu(expanded = attachMenu, onDismissRequest = { attachMenu = false }) {
                    DropdownMenuItem(text = { Text("Files") }, onClick = {
                        attachMenu = false
                        if (canSendFiles) {
                            appLock?.expectExternalReturn()
                            pick.launch(arrayOf("*/*"))
                        } else {
                            fileSent = false
                            fileError = PRO_REQUIRED_MESSAGE
                        }
                    })
                    DropdownMenuItem(text = { Text("Poll") }, onClick = {
                        attachMenu = false
                        onStartPoll()
                    })
                }
            }
            if (recording) {
                Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
                    Text("● ", color = colors.danger)
                    Text(
                        "Recording ${recordedMs / 60_000}:${"%02d".format((recordedMs / 1000) % 60)}",
                        color = colors.text,
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier.weight(1f),
                    )
                    TextButton(onClick = {
                        recording = false
                        recorder.cancel()
                    }) { Text("Cancel", color = colors.text2) }
                    TextButton(onClick = { sendRecording() }) { Text("Send", fontWeight = FontWeight.SemiBold) }
                }
                return@Row
            }
            OutlinedTextField(
                state = draft,
                placeholder = { Text(if (editing != null) "Edit message" else "Message") },
                lineLimits = TextFieldLineLimits.MultiLine(maxHeightInLines = 5),
                inputTransformation = typingSignal,
                shape = RoundedCornerShape(22.dp),
                trailingIcon = {
                    IconButton(onClick = { pickingEmoji = true }) {
                        Icon(painterResource(R.drawable.ui_smile), contentDescription = "Emoji", tint = colors.text2)
                    }
                },
                modifier = Modifier.weight(1f).contentReceiver(pictureReceiver),
            )
            if (text.isBlank() && editing == null) {
                IconButton(enabled = fileStage == null, onClick = {
                    when {
                        !canSendFiles -> {
                            fileSent = false
                            fileError = PRO_REQUIRED_MESSAGE
                        }
                        ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED ->
                            startRecording()
                        else -> micPermission.launch(Manifest.permission.RECORD_AUDIO)
                    }
                }) {
                    Icon(painterResource(R.drawable.ic_mic), contentDescription = "Record a voice message", tint = colors.text2)
                }
                return@Row
            }
            IconButton(
                enabled = canSend,
                onClick = {
                    sending = true
                    onSend(draft.text.toString()) { sent ->
                        sending = false
                        if (sent) draft.clearText()
                    }
                },
            ) {
                Icon(
                    painterResource(R.drawable.ic_send),
                    contentDescription = if (sending) "Sending" else "Send",
                    tint = if (canSend) colors.accent else colors.text3,
                )
            }
        }
    }

    if (pickingEmoji) {
        EmojiPickerDialog(
            onPick = { emoji ->
                pickingEmoji = false
                draft.edit {
                    val at = selection
                    replace(at.min, at.max, emoji)
                    selection = TextRange(at.min + emoji.length)
                }
                onTyping()
            },
            onDismiss = { pickingEmoji = false },
        )
    }
}

/** A picked or shared file's name and size; one this app can't look up is still listed (and fails clearly when sent). */
private fun describeShared(context: android.content.Context, uri: Uri): PendingFile =
    runCatching { describePicked(context, uri) }.getOrElse { PendingFile(uri, "file", null) }

/** Null when it can't be read; without bytes when it's over [MAX_PASTED_BYTES]. */
private fun readPastedPicture(context: android.content.Context, uri: Uri, mimeType: String): PendingFile? = runCatching {
    val described = describeShared(context, uri)
    val extension = when (mimeType.lowercase()) {
        "image/jpeg" -> "jpg"
        "image/png" -> "png"
        "image/gif" -> "gif"
        "image/webp" -> "webp"
        "image/bmp" -> "bmp"
        else -> mimeType.substringAfter('/').filter { it.isLetterOrDigit() }.take(5).ifEmpty { "img" }
    }
    val name = described.name.takeIf { it != "file" && '.' in it } ?: "Pasted picture.$extension"
    val bytes = context.contentResolver.openInputStream(uri)?.use { input ->
        val out = java.io.ByteArrayOutputStream()
        val buffer = ByteArray(64 * 1024)
        var total = 0L
        while (true) {
            val read = input.read(buffer)
            if (read < 0) break
            total += read
            if (total > MAX_PASTED_BYTES) return@runCatching PendingFile(null, name, total, mimeType, null)
            out.write(buffer, 0, read)
        }
        out.toByteArray()
    } ?: return@runCatching null
    PendingFile(null, FileMessage.sanitizeFilename(name), bytes.size.toLong(), mimeType, bytes)
}.getOrNull()
