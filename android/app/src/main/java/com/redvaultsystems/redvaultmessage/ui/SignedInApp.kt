package com.redvaultsystems.redvaultmessage.ui

import com.redvaultsystems.redvaultmessage.data.api.TOR_UNAVAILABLE_MESSAGE
import com.redvaultsystems.redvaultmessage.tor.TorState
import com.redvaultsystems.redvaultmessage.ui.settings.TorStatusStrip
import com.redvaultsystems.redvaultmessage.ui.settings.NetworkRoutingScreen
import android.Manifest
import android.os.Build
import androidx.activity.compose.LocalActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import com.redvaultsystems.redvaultmessage.push.PushMode
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.rememberCoroutineScope
import androidx.activity.compose.BackHandler
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.ProcessLifecycleOwner
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.repeatOnLifecycle
import com.redvaultsystems.redvaultmessage.data.api.TransparencyEventDto
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.ui.platform.LocalContext
import com.redvaultsystems.redvaultmessage.calls.CallPhase
import com.redvaultsystems.redvaultmessage.calls.GroupCallLink
import com.redvaultsystems.redvaultmessage.calls.GroupCallManager
import com.redvaultsystems.redvaultmessage.calls.GroupPhase
import com.redvaultsystems.redvaultmessage.ui.calls.GroupCallBar
import com.redvaultsystems.redvaultmessage.ui.calls.GroupCallScreen
import com.redvaultsystems.redvaultmessage.ui.calls.CallBar
import com.redvaultsystems.redvaultmessage.ui.calls.CallScreen
import com.redvaultsystems.redvaultmessage.ui.calls.CallsTab
import com.redvaultsystems.redvaultmessage.ui.calls.rememberCallPermissions
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import com.redvaultsystems.redvaultmessage.ui.chats.ChatList
import com.redvaultsystems.redvaultmessage.ui.chats.ChatsScreen
import com.redvaultsystems.redvaultmessage.ui.chats.ShareToScreen
import com.redvaultsystems.redvaultmessage.ui.chats.SharedItems
import androidx.compose.runtime.key
import com.redvaultsystems.redvaultmessage.ui.stories.StoriesTray
import com.redvaultsystems.redvaultmessage.ui.stories.StoryComposerScreen
import com.redvaultsystems.redvaultmessage.ui.stories.StoryHighlightsRow
import com.redvaultsystems.redvaultmessage.ui.stories.StorySettingsScreen
import com.redvaultsystems.redvaultmessage.ui.stories.StoryViewerScreen
import com.redvaultsystems.redvaultmessage.ui.stories.ViewerGroup
import com.redvaultsystems.redvaultmessage.ui.chats.ChatsState
import com.redvaultsystems.redvaultmessage.ui.chats.MessageRequestsScreen
import com.redvaultsystems.redvaultmessage.ui.inbox.InboxScreen
import com.redvaultsystems.redvaultmessage.ui.chats.SharedMediaScreen
import com.redvaultsystems.redvaultmessage.ui.files.IncomingScreen
import com.redvaultsystems.redvaultmessage.ui.files.ShareLinkEditScreen
import com.redvaultsystems.redvaultmessage.ui.files.SharedLinksScreen
import com.redvaultsystems.redvaultmessage.ui.files.ShareLinkScreen
import com.redvaultsystems.redvaultmessage.ui.files.FileDetailScreen
import com.redvaultsystems.redvaultmessage.ui.files.FileKeyScreen
import com.redvaultsystems.redvaultmessage.ui.files.FileKeysScreen
import com.redvaultsystems.redvaultmessage.ui.files.FilesTab
import com.redvaultsystems.redvaultmessage.ui.files.KeyCreateSheet
import com.redvaultsystems.redvaultmessage.ui.files.KeyImportSheet
import com.redvaultsystems.redvaultmessage.ui.files.KeyPasswordSheet
import com.redvaultsystems.redvaultmessage.ui.files.ShareContact
import com.redvaultsystems.redvaultmessage.ui.files.ShareFileScreen
import com.redvaultsystems.redvaultmessage.ui.files.UnlockFileSheet
import com.redvaultsystems.redvaultmessage.ui.files.UploadLockSheet
import com.redvaultsystems.redvaultmessage.ui.contacts.ContactCodeScreen
import com.redvaultsystems.redvaultmessage.ui.contacts.ContactScreen
import com.redvaultsystems.redvaultmessage.ui.contacts.ContactsTab
import com.redvaultsystems.redvaultmessage.ui.contacts.contactsOf
import com.redvaultsystems.redvaultmessage.ui.passwords.PasswordEditSheet
import com.redvaultsystems.redvaultmessage.ui.passwords.PasswordItemScreen
import com.redvaultsystems.redvaultmessage.ui.passwords.PasswordShareSheet
import com.redvaultsystems.redvaultmessage.ui.passwords.PasswordsTab
import com.redvaultsystems.redvaultmessage.ui.theme.RedvaultTheme
import com.redvaultsystems.redvaultmessage.ui.components.HideFromRecents
import com.redvaultsystems.redvaultmessage.ui.components.SecureWindow
import com.redvaultsystems.redvaultmessage.ui.components.TouchGuard
import com.redvaultsystems.redvaultmessage.ui.chats.ThreadScreen
import com.redvaultsystems.redvaultmessage.ui.communities.CommunitiesPage
import com.redvaultsystems.redvaultmessage.ui.communities.CommunitiesScreen
import com.redvaultsystems.redvaultmessage.ui.communities.CommunityScreen
import com.redvaultsystems.redvaultmessage.ui.friends.AddFriendScreen
import com.redvaultsystems.redvaultmessage.ui.friends.FriendRequestsScreen
import com.redvaultsystems.redvaultmessage.ui.channels.ChannelScreen
import com.redvaultsystems.redvaultmessage.ui.channels.ChannelsTab
import com.redvaultsystems.redvaultmessage.ui.groups.GroupInfoScreen
import com.redvaultsystems.redvaultmessage.ui.groups.JoinGroupScreen
import com.redvaultsystems.redvaultmessage.ui.groups.NewGroupScreen
import com.redvaultsystems.redvaultmessage.ui.settings.ChangePasswordScreen
import com.redvaultsystems.redvaultmessage.ui.settings.ChangeUsernameScreen
import com.redvaultsystems.redvaultmessage.ui.settings.DeleteAccountScreen
import com.redvaultsystems.redvaultmessage.ui.settings.MfaScreen
import com.redvaultsystems.redvaultmessage.ui.lock.AppLockSettingsScreen
import com.redvaultsystems.redvaultmessage.ui.recovery.RecoveryKeySettingsScreen
import com.redvaultsystems.redvaultmessage.ui.lock.LocalAppLock
import com.redvaultsystems.redvaultmessage.ui.lock.LockScreen
import com.redvaultsystems.redvaultmessage.ui.settings.BlockedUsersScreen
import com.redvaultsystems.redvaultmessage.ui.settings.DeviceKeysScreen
import com.redvaultsystems.redvaultmessage.ui.settings.DeviceKeyChangedNotice
import com.redvaultsystems.redvaultmessage.ui.settings.DeviceKeySafetyNumberSection
import com.redvaultsystems.redvaultmessage.ui.settings.DeviceKeySafetyState
import com.redvaultsystems.redvaultmessage.ui.settings.LinkedDevicesScreen
import com.redvaultsystems.redvaultmessage.ui.settings.RelayServersScreen
import com.redvaultsystems.redvaultmessage.ui.settings.TransparencyLogScreen
import com.redvaultsystems.redvaultmessage.ui.settings.PrivacyModeScreen
import com.redvaultsystems.redvaultmessage.ui.settings.SettingsScreen
import com.redvaultsystems.redvaultmessage.ui.settings.isPaidTier
import com.redvaultsystems.redvaultmessage.ui.verify.MessageDetailsScreen
import com.redvaultsystems.redvaultmessage.ui.verify.TransparencyFeedScreen
import com.redvaultsystems.redvaultmessage.ui.verify.VerifyScreen
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch

/** Screens reachable while signed in. Encoded as strings so the back stack survives rotation. */
internal sealed interface Route {
    /** The tabs (Chats, Calls, Contacts, Channels, Files, Passwords). */
    data class Home(val tab: String = Tabs.CHATS) : Route
    data class Thread(val conversationId: String) : Route
    data object AddFriend : Route
    data object FriendRequests : Route
    /** Chats from people who aren't friends, waiting to be accepted or ignored. */
    data object MessageRequests : Route
    /** Messages and polls from the Redvault team. */
    data object Inbox : Route
    data object Settings : Route
    data object ChangeUsername : Route
    data object ChangePassword : Route
    data object Mfa : Route
    data class Verify(val conversationId: String) : Route
    data class MessageDetails(val conversationId: String, val messageId: String) : Route
    data object Transparency : Route
    data object AppLockSettings : Route
    data object PrivacyMode : Route
    data object Blocked : Route
    data object RecoveryKeySettings : Route
    data object DeleteAccount : Route
    data object NewGroup : Route
    data class GroupInfo(val conversationId: String) : Route
    /** Someone's group invite link: [secret] is the base64url secret from its fragment (only ever on this phone). */
    data class JoinGroup(val linkId: String, val secret: String) : Route
    /** A channel; [secret] is a private channel's link secret (base64url) when it was opened from its link, else empty. */
    data class Channel(val id: String, val secret: String = "") : Route

    // Stories (only reachable while the server has them on)
    /** [authorId] is a friend's user id, or "me" for your own. */
    data class StoryViewer(val authorId: String) : Route
    data object StoryComposer : Route
    data object StorySettings : Route

    // Files
    data class FileDetail(val id: String) : Route
    data class ShareFile(val id: String) : Route
    /** A public share link to a file or folder (docs/public-share-links.md). */
    data class ShareLink(val id: String) : Route
    data object SharedLinks : Route
    /** [mode]: "settings", "password" or "regenerate". */
    data class ShareLinkEdit(val id: String, val mode: String) : Route
    data object Incoming : Route
    data object UploadLock : Route
    data class ChangeLock(val id: String, val mode: String) : Route
    data class UnlockFile(val id: String, val then: String) : Route
    data object FileKeys : Route
    data class FileKey(val id: String) : Route
    data object KeyCreate : Route
    data object KeyImport : Route
    data class KeyPassword(val id: String, val action: String) : Route

    // Passwords
    data class PasswordItem(val id: String) : Route
    /** [id] empty for a new item of [type]. */
    data class PasswordEdit(val id: String, val type: String) : Route
    data class PasswordShare(val id: String) : Route

    // Contacts
    data class Contact(val userId: String) : Route
    data object ContactCode : Route
    /** Add a friend with the handle filled in (a scanned contact code). */
    data class AddFriendAs(val handle: String) : Route
    data class SharedMedia(val conversationId: String) : Route
    /** Pick the chat for files or text shared from another app (they're held in memory, never in this route). */
    data object ShareTo : Route

    // Settings › security
    data object LinkedDevices : Route
    data object DeviceKeys : Route
    data object RelayServers : Route
    data object TransparencyLog : Route
    data object NetworkRouting : Route

    // Communities: [page] is a CommunitiesPage key (discover, mine, memberships).
    data class Communities(val page: String = "discover") : Route
    data class Community(val id: String) : Route

    // Fixed names rather than class names: R8 renames classes in release builds.
    fun encode(): String = when (this) {
        is Home -> "home:$tab"
        is Thread -> "thread:$conversationId"
        AddFriend -> "addFriend"
        FriendRequests -> "friendRequests"
        MessageRequests -> "messageRequests"
        Inbox -> "inbox"
        Settings -> "settings"
        ChangeUsername -> "changeUsername"
        ChangePassword -> "changePassword"
        Mfa -> "mfa"
        is Verify -> "verify:$conversationId"
        is MessageDetails -> "details:$conversationId:$messageId"
        Transparency -> "transparency"
        AppLockSettings -> "appLock"
        PrivacyMode -> "privacyMode"
        Blocked -> "blocked"
        RecoveryKeySettings -> "recoveryKey"
        DeleteAccount -> "deleteAccount"
        NewGroup -> "newGroup"
        is GroupInfo -> "groupInfo:$conversationId"
        is JoinGroup -> "joinGroup:$linkId:$secret"
        is Channel -> if (secret.isEmpty()) "channel:$id" else "channel:$id:$secret"
        is StoryViewer -> "storyViewer:$authorId"
        StoryComposer -> "storyComposer"
        StorySettings -> "storySettings"
        is FileDetail -> "file:$id"
        is ShareFile -> "shareFile:$id"
        is ShareLink -> "shareLink:$id"
        SharedLinks -> "sharedLinks"
        is ShareLinkEdit -> "shareLinkEdit:$id:$mode"
        Incoming -> "incoming"
        UploadLock -> "uploadLock"
        is ChangeLock -> "changeLock:$id:$mode"
        is UnlockFile -> "unlockFile:$id:$then"
        FileKeys -> "fileKeys"
        is FileKey -> "fileKey:$id"
        KeyCreate -> "keyCreate"
        KeyImport -> "keyImport"
        is KeyPassword -> "keyPassword:$id:$action"
        is PasswordItem -> "password:$id"
        is PasswordEdit -> "passwordEdit:$type:$id"
        is PasswordShare -> "passwordShare:$id"
        is Contact -> "contact:$userId"
        ContactCode -> "contactCode"
        is AddFriendAs -> "addFriendAs:$handle"
        is SharedMedia -> "sharedMedia:$conversationId"
        ShareTo -> "shareTo"
        LinkedDevices -> "linkedDevices"
        DeviceKeys -> "deviceKeys"
        RelayServers -> "relayServers"
        TransparencyLog -> "transparencyLog"
        NetworkRouting -> "networkRouting"
        is Communities -> "communities:$page"
        is Community -> "community:$id"
    }

    companion object {
        fun decode(value: String): Route = when {
            value.startsWith("thread:") -> Thread(value.removePrefix("thread:"))
            value.startsWith("verify:") -> Verify(value.removePrefix("verify:"))
            value.startsWith("details:") -> value.removePrefix("details:").split(":").let { parts ->
                if (parts.size == 2) MessageDetails(parts[0], parts[1]) else Home()
            }
            value == "transparency" -> Transparency
            value == "appLock" -> AppLockSettings
            value == "privacyMode" -> PrivacyMode
            value == "blocked" -> Blocked
            value == "recoveryKey" -> RecoveryKeySettings
            value == "deleteAccount" -> DeleteAccount
            value == "newGroup" -> NewGroup
            value.startsWith("groupInfo:") -> GroupInfo(value.removePrefix("groupInfo:"))
            value.startsWith("joinGroup:") -> value.removePrefix("joinGroup:").split(":", limit = 2).let { JoinGroup(it[0], it.getOrElse(1) { "" }) }
            value.startsWith("channel:") -> value.removePrefix("channel:").split(":", limit = 2).let { Channel(it[0], it.getOrElse(1) { "" }) }
            value.startsWith("storyViewer:") -> StoryViewer(value.removePrefix("storyViewer:"))
            value == "storyComposer" -> StoryComposer
            value == "storySettings" -> StorySettings
            value == "addFriend" -> AddFriend
            value == "friendRequests" -> FriendRequests
            value == "messageRequests" -> MessageRequests
            value == "inbox" -> Inbox
            value == "settings" -> Settings
            value == "changeUsername" -> ChangeUsername
            value == "changePassword" -> ChangePassword
            value == "mfa" -> Mfa
            value.startsWith("home:") -> Home(value.removePrefix("home:"))
            value.startsWith("file:") -> FileDetail(value.removePrefix("file:"))
            value.startsWith("shareFile:") -> ShareFile(value.removePrefix("shareFile:"))
            value.startsWith("shareLink:") -> ShareLink(value.removePrefix("shareLink:"))
            value == "sharedLinks" -> SharedLinks
            value.startsWith("shareLinkEdit:") -> value.removePrefix("shareLinkEdit:").split(":").let { ShareLinkEdit(it[0], it.getOrElse(1) { "settings" }) }
            value == "incoming" -> Incoming
            value == "uploadLock" -> UploadLock
            value.startsWith("changeLock:") -> value.removePrefix("changeLock:").split(":").let { ChangeLock(it[0], it.getOrElse(1) { "" }) }
            value.startsWith("unlockFile:") -> value.removePrefix("unlockFile:").split(":").let { UnlockFile(it[0], it.getOrElse(1) { "" }) }
            value == "fileKeys" -> FileKeys
            value.startsWith("fileKey:") -> FileKey(value.removePrefix("fileKey:"))
            value == "keyCreate" -> KeyCreate
            value == "keyImport" -> KeyImport
            value.startsWith("password:") -> PasswordItem(value.removePrefix("password:"))
            value.startsWith("passwordEdit:") -> value.removePrefix("passwordEdit:").split(":", limit = 2).let { PasswordEdit(it.getOrElse(1) { "" }, it[0]) }
            value.startsWith("passwordShare:") -> PasswordShare(value.removePrefix("passwordShare:"))
            value.startsWith("contact:") -> Contact(value.removePrefix("contact:"))
            value == "contactCode" -> ContactCode
            value.startsWith("addFriendAs:") -> AddFriendAs(value.removePrefix("addFriendAs:"))
            value.startsWith("sharedMedia:") -> SharedMedia(value.removePrefix("sharedMedia:"))
            value == "shareTo" -> ShareTo
            value == "linkedDevices" -> LinkedDevices
            value == "deviceKeys" -> DeviceKeys
            value == "relayServers" -> RelayServers
            value == "transparencyLog" -> TransparencyLog
            value == "networkRouting" -> NetworkRouting
            value.startsWith("communities:") -> Communities(value.removePrefix("communities:"))
            value.startsWith("community:") -> Community(value.removePrefix("community:"))
            value.startsWith("keyPassword:") -> value.removePrefix("keyPassword:").split(":").let { KeyPassword(it[0], it.getOrElse(1) { "change" }) }
            else -> Home()
        }
    }
}

@Composable
fun SignedInApp(
    session: SignedInSession,
    loadTransparencyFeed: suspend (limit: Int) -> List<TransparencyEventDto>,
    onLogout: () -> Unit,
    /** A channel to open, from a tapped new-post notification. */
    openChannelRequests: MutableStateFlow<String?> = MutableStateFlow(null),
    /** A tapped "new message" notification (it names no chat). */
    openChatRequests: MutableStateFlow<Boolean> = MutableStateFlow(false),
    /** Files or text shared from another app, waiting for a chat to go to. */
    shareRequests: MutableStateFlow<SharedItems?> = MutableStateFlow(null),
) {
    CompositionLocalProvider(LocalAppLock provides session.lock) {
        SignedInContent(session, loadTransparencyFeed, onLogout, openChannelRequests, openChatRequests, shareRequests)
    }
}

@Composable
private fun SignedInContent(
    session: SignedInSession,
    loadTransparencyFeed: suspend (limit: Int) -> List<TransparencyEventDto>,
    onLogout: () -> Unit,
    openChannelRequests: MutableStateFlow<String?>,
    openChatRequests: MutableStateFlow<Boolean>,
    shareRequests: MutableStateFlow<SharedItems?>,
) {
    val chats by session.chats.state.collectAsStateWithLifecycle()
    val account by session.account.state.collectAsStateWithLifecycle()
    val filesState by session.files.state.collectAsStateWithLifecycle()
    val sharesState by session.shares.state.collectAsStateWithLifecycle()
    val vaultState by session.vault.state.collectAsStateWithLifecycle()
    val live by session.live.state.collectAsStateWithLifecycle()
    val stories by session.stories.state.collectAsStateWithLifecycle()
    val drafts by session.chats.drafts.collectAsStateWithLifecycle()
    // Shared from another app: waiting for a chat (Route.ShareTo), then for that chat's message box. Memory only.
    var pendingShare by remember { mutableStateOf<SharedItems?>(null) }
    var shareInto by remember { mutableStateOf<Pair<String, SharedItems>?>(null) }
    // Files picked for upload, waiting on the upload sheet (not saved across process death: pick them again).
    var pendingUploads by remember { mutableStateOf<Pair<List<android.net.Uri>, String>>(emptyList<android.net.Uri>() to "") }
    val lifecycleOwner = LocalLifecycleOwner.current
    var hideContent by remember { mutableStateOf(session.settings.hideContent) }
    var ringtone by remember { mutableStateOf(session.settings.ringtone) }
    var echoCancellation by remember { mutableStateOf(session.settings.echoCancellation) }

    var encodedStack by rememberSaveable { mutableStateOf(Route.Home().encode()) }
    val stack = encodedStack.split('\n').map(Route::decode)
    fun push(route: Route) {
        encodedStack = (stack + route).joinToString("\n") { it.encode() }
    }
    fun pop() {
        if (stack.size > 1) encodedStack = stack.dropLast(1).joinToString("\n") { it.encode() }
    }
    /** Replaces everything above the chat list, e.g. accepting a request lands in the new chat. */
    fun resetTo(route: Route) {
        encodedStack = listOf(Route.Home(), route).joinToString("\n") { it.encode() }
    }
    /** Replaces the top screen (e.g. a finished sheet opening the next one). */
    fun replaceTop(route: Route) {
        encodedStack = (stack.dropLast(1) + route).joinToString("\n") { it.encode() }
    }
    /** Switches tabs (the tabs are the bottom of the stack). */
    fun selectTab(tab: String) {
        encodedStack = Route.Home(tab).encode()
    }

    // Refresh only while the app is on screen: it stops when backgrounded
    // and resumes, with an immediate refresh, when it comes back.
    LaunchedEffect(session) {
        lifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
            launch { session.chats.pollConversations() }
            launch { session.account.poll() }
            launch { session.files.poll() }
            // The first thing this does is ask whether the server has Stories on; with them off it shows and fetches nothing.
            launch { session.stories.poll() }
            launch { session.shares.poll() }
            // Being on the live channel is what "online" means, so only while the app is on screen.
            launch { session.live.holdWhileActive("screen") }
            launch { session.live.tick() }
            // Communities: deliver keys to members who are waiting, rotate after someone left.
            launch { session.communities.maintainKeys(onChanged = session.chats::refreshNow) }
            // Coming back on screen: look for new posts in followed channels at once.
            session.channels.checkNow()
        }
    }
    // Followed channels are checked for new posts every couple of minutes while the app runs (the badge and notifications).
    LaunchedEffect(session) { session.channels.startWatching() }
    val unreadChannels by session.channels.unread.collectAsStateWithLifecycle()
    // The team's messages and polls: checked every minute while the app is on screen (the Settings badge).
    LaunchedEffect(session) { session.inbox.poll() }
    val inboxState by session.inbox.state.collectAsStateWithLifecycle()
    // A tapped new-post notification opens its channel (behind the lock screen when app lock is on).
    LaunchedEffect(openChannelRequests) {
        openChannelRequests.collect { id ->
            if (id == null) return@collect
            openChannelRequests.value = null
            // The stack as it is now, not as it was when this effect started.
            val current = encodedStack.split('\n').map(Route::decode)
            if ((current.last() as? Route.Channel)?.id != id) encodedStack = (current + Route.Channel(id)).joinToString("\n") { it.encode() }
        }
    }
    // A tapped "new message" notification: the push names no chat, so this phone looks. Exactly one chat
    // with unread messages opens; otherwise the chat list shows.
    LaunchedEffect(openChatRequests) {
        openChatRequests.collect { requested ->
            if (!requested) return@collect
            openChatRequests.value = false
            session.chats.refreshNow()
            val target = ChatList.chatToOpen(session.chats.state.value.conversations)
            val current = encodedStack.split('\n').map(Route::decode)
            when {
                target != null && (current.last() as? Route.Thread)?.conversationId != target ->
                    encodedStack = listOf(Route.Home(), Route.Thread(target)).joinToString("\n") { it.encode() }
                target == null -> encodedStack = Route.Home(Tabs.CHATS).encode()
            }
        }
    }
    // Shared from another app: pick the chat next (the files stay in memory, never in the back stack).
    LaunchedEffect(shareRequests) {
        shareRequests.collect { items ->
            if (items == null) return@collect
            shareRequests.value = null
            pendingShare = items
            val current = encodedStack.split('\n').map(Route::decode)
            if (current.last() != Route.ShareTo) encodedStack = (current + Route.ShareTo).joinToString("\n") { it.encode() }
        }
    }
    // Older messages loaded in a chat stay only while that chat is somewhere in the back stack.
    LaunchedEffect(encodedStack) {
        session.chats.keepWindows(stack.filterIsInstance<Route.Thread>().mapTo(HashSet()) { it.conversationId })
    }
    // Online status is reciprocal: seen only while shared (the server's setting).
    LaunchedEffect(session, account.profile.showOnlineStatus) {
        session.live.setPresenceEnabled(account.profile.showOnlineStatus == true)
    }

    // Content-free pushes (push/PushRegistrar.kt): ask once for the notification permission Android 13+
    // requires, then pick the route from here, the signed-in screen's activity, since choosing a UnifiedPush
    // distributor can bring up Android's chooser. Without a distributor: the background connection.
    // Strict no-push still allows the background connection with Tor routing (push/PushRegistrar.kt).
    var notificationsOn by remember { mutableStateOf(session.push.enabled && (!session.push.strictNoPush || session.push.torRouted)) }
    var strictNoPush by remember { mutableStateOf(session.push.strictNoPush) }
    var backgroundConnection by remember { mutableStateOf(session.push.backgroundConnection) }
    val pushMode by session.push.mode.collectAsStateWithLifecycle()
    // Tor routing only: the gateway's state, for the strip above the tabs, the thread banner and Settings.
    val torStatus = session.tor?.status?.collectAsStateWithLifecycle()?.value
    val scope = rememberCoroutineScope()
    val activity = LocalActivity.current
    fun registerPush() {
        val host = activity ?: return
        scope.launch { session.push.register(host) }
    }
    // Whatever the answer: allowed, the background connection can notify; refused, it doesn't start.
    val askPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { registerPush() }
    LaunchedEffect(session) {
        if (session.push.enabled && (!session.push.strictNoPush || session.push.torRouted) && Build.VERSION.SDK_INT >= 33 && !session.settings.askedForNotifications) {
            session.settings.askedForNotifications = true
            askPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        } else {
            registerPush()
        }
    }
    // Back in the foreground: start a background connection Android wouldn't start from the background,
    // switch to a push app installed meanwhile, and recount push apps for Settings.
    var foregrounds by remember { mutableStateOf(0) }
    DisposableEffect(session) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_START) {
                foregrounds++
                session.push.resumeIfNeeded()
                if (session.push.mode.value == PushMode.BackgroundConnection && session.push.distributors().isNotEmpty()) registerPush()
            }
        }
        val processLifecycle = ProcessLifecycleOwner.get().lifecycle
        processLifecycle.addObserver(observer)
        onDispose { processLifecycle.removeObserver(observer) }
    }

    // "Hide app content" covers the whole app; screens with secrets on them are covered whatever it says.
    SecureWindow(hideContent)
    SecureWindow(stack.last().holdsSecrets())
    TouchGuard(stack.last().guardsTouches())
    // With app lock on, chats don't stay readable in the Recents thumbnail while the app is locked. The lock
    // screen only appears on coming back, so what's on screen on leaving is what Recents would keep.
    val appLockOn by session.lock.enabled.collectAsStateWithLifecycle()
    HideFromRecents(appLockOn)

    // Auto-lock: watch the whole app go to the background and come back
    // (process-wide, so rotating the phone doesn't count as leaving).
    DisposableEffect(session) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_STOP -> session.lock.onBackground()
                Lifecycle.Event.ON_START -> session.lock.onForeground()
                else -> {}
            }
        }
        val processLifecycle = ProcessLifecycleOwner.get().lifecycle
        processLifecycle.addObserver(observer)
        onDispose { processLifecycle.removeObserver(observer) }
    }
    val locked by session.lock.locked.collectAsStateWithLifecycle()
    if (locked) {
        LockScreen(session.lock, onLockedOut = onLogout)
        return
    }

    // A call shows over everything; minimized, a bar above the app takes you back.
    val callsState by session.calls.state.collectAsStateWithLifecycle()
    val groupState by session.groupCalls.state.collectAsStateWithLifecycle()
    var callMinimized by rememberSaveable { mutableStateOf(false) }
    val oneToOneCall = callsState.call
    // Only one call is ever on; a closing banner of the other kind doesn't count.
    val groupCall = groupState.call?.takeIf { oneToOneCall == null || oneToOneCall.phase == CallPhase.Ended || it.phase != GroupPhase.Ended }
    val activeCall = oneToOneCall?.takeIf { groupCall == null || groupCall.phase == GroupPhase.Ended }
    LaunchedEffect(activeCall == null, activeCall?.phase == CallPhase.Incoming, groupCall == null, groupCall?.phase == GroupPhase.Incoming) {
        if ((activeCall == null && groupCall == null) || activeCall?.phase == CallPhase.Incoming || groupCall?.phase == GroupPhase.Incoming) callMinimized = false
    }
    val context = LocalContext.current
    fun toast(message: String) = android.widget.Toast.makeText(context, message, android.widget.Toast.LENGTH_LONG).show()
    if (groupCall != null && !callMinimized) {
        BackHandler { callMinimized = true }
        GroupCallScreen(
            calls = session.groupCalls,
            call = groupCall,
            onMinimize = { callMinimized = true },
            onOpenChat = { id -> if ((stack.last() as? Route.Thread)?.conversationId != id) push(Route.Thread(id)) },
        )
        return
    }
    if (activeCall != null && !callMinimized) {
        BackHandler { callMinimized = true }
        CallScreen(
            calls = session.calls,
            call = activeCall,
            onMinimize = { callMinimized = true },
            onOpenChat = { id -> if ((stack.last() as? Route.Thread)?.conversationId != id) push(Route.Thread(id)) },
        )
        return
    }
    val askCallPermissions = rememberCallPermissions { message -> toast(message) }
    fun call(conversationId: String, kind: String) {
        val conversation = chats.conversations.firstOrNull { it.id == conversationId } ?: return
        askCallPermissions(kind == "video") { camera -> session.calls.startCall(conversation, if (kind == "video" && camera) "video" else "voice") }
    }
    /** Starts a call in a group, or joins the one going on there. */
    fun groupCallIn(conversationId: String, kind: String) {
        val conversation = chats.conversations.firstOrNull { it.id == conversationId && it.isGroup } ?: return
        askCallPermissions(kind == "video") { _ -> session.groupCalls.start(conversation, kind)?.let(::toast) }
    }
    /** Joins from a pasted link or message: a group call's, or else someone's call link. */
    fun joinFromLink(text: String, onResult: (String?) -> Unit) {
        val groupCallId = GroupCallLink.parse(text)
        if (groupCallId != null) {
            askCallPermissions(false) { _ -> session.groupCalls.joinByLink(groupCallId, onResult) }
        } else {
            askCallPermissions(false) { _ -> onResult(session.calls.joinLink(text)) }
        }
    }

    BackHandler(enabled = stack.size > 1) { pop() }

    val incoming = account.friendRequests.incoming.size
    Column(Modifier.fillMaxSize().background(RedvaultTheme.colors.bg)) {
    if (groupCall != null) GroupCallBar(groupCall) { callMinimized = false }
    else if (activeCall != null) CallBar(activeCall) { callMinimized = false }
    Box(Modifier.weight(1f)) {
    when (val route = stack.last()) {
        is Route.Home -> HomeShell(
            route.tab,
            // Muted chats stay out of the badge, as on the desktop.
            unreadChats = ChatList.unreadForBadge(chats.conversations, live.muted),
            unreadChannels = unreadChannels,
            incomingFiles = sharesState.incoming.size,
            onTab = ::selectTab,
            status = torStatus?.let { status ->
                {
                    TorStatusStrip(
                        status, queued = chats.queued, pausedUploads = filesState.pausedUploads,
                        callActive = activeCall != null || groupCall != null,
                    )
                }
            },
        ) {
            when (route.tab) {
                Tabs.CALLS -> CallsTab(
                    state = callsState,
                    conversations = chats.conversations,
                    onCall = ::call,
                    onNewCall = { selectTab(Tabs.CONTACTS) },
                    onCreateLink = session.calls::createLink,
                    onDeleteLink = session.calls::deleteLink,
                    onJoinLink = ::joinFromLink,
                    onClearHistory = session.calls::clearHistory,
                    onRefreshLinks = session.calls::refreshLinks,
                    onClearLinkMessages = session.calls::clearLinkMessages,
                    tier = account.profile.subscriptionTier,
                )
                Tabs.CONTACTS -> ContactsTab(
                    conversations = chats.conversations,
                    live = live,
                    onOpen = { push(Route.Contact(it)) },
                    onAdd = { handle -> push(if (handle.isEmpty()) Route.AddFriend else Route.AddFriendAs(handle)) },
                    onShareCode = { push(Route.ContactCode) },
                    onCall = if (callsState.available) ::call else null,
                )
                Tabs.CHANNELS -> ChannelsTab(
                    channels = session.channels,
                    premium = account.profile.subscriptionTier == "premium",
                    onOpen = { link -> push(Route.Channel(link.id, link.secret.orEmpty())) },
                )
                Tabs.FILES -> FilesTab(
                    files = session.files,
                    state = filesState,
                    onOpenFile = { push(Route.FileDetail(it)) },
                    onOpenKeys = { push(Route.FileKeys) },
                    incomingCount = sharesState.incoming.size,
                    activeLinks = sharesState.activeLinks,
                    onShareLink = { push(Route.ShareLink(it)) },
                    onOpenLinks = { push(Route.SharedLinks) },
                    onOpenIncoming = { push(Route.Incoming) },
                    onUpload = { uris, parent ->
                        pendingUploads = uris to parent
                        push(Route.UploadLock)
                    },
                )
                Tabs.PASSWORDS -> PasswordsTab(
                    vault = session.vault,
                    state = vaultState,
                    onOpen = { push(Route.PasswordItem(it)) },
                    onNew = { push(Route.PasswordEdit("", it)) },
                )
                else -> ChatsScreen(
                    state = chats,
                    selfHandle = account.profile.handle,
                    selfAvatarUrl = account.profile.avatarUrl,
                    incomingRequests = incoming,
                    online = live.online,
                    onOpen = { push(Route.Thread(it)) },
                    onAddFriend = { push(Route.AddFriend) },
                    onNewGroup = { push(Route.NewGroup) },
                    onOpenRequests = { push(Route.FriendRequests) },
                    onOpenSettings = { push(Route.Settings) },
                    onJoinGroupLink = { linkId, secret -> push(Route.JoinGroup(linkId, secret)) },
                    onOpenMessageRequests = { push(Route.MessageRequests) },
                    onOpenCommunities = { push(Route.Communities()) },
                    favorites = live.favorites,
                    muted = live.muted,
                    typingIn = live.typing.filterValues { it.isNotEmpty() }.keys,
                    drafts = drafts,
                    stories = {
                        StoriesTray(
                            state = stories,
                            selfHandle = account.profile.handle,
                            selfAvatarUrl = account.profile.avatarUrl,
                            onOpenOwn = { push(Route.StoryViewer("me")) },
                            onCompose = { push(Route.StoryComposer) },
                            onOpenAuthor = { push(Route.StoryViewer(it)) },
                        )
                    },
                )
            }
        }

        is Route.Thread -> {
            val conversation = chats.conversations.firstOrNull { it.id == route.conversationId }
            if (conversation == null) {
                // Not loaded yet (or gone): a spinner until it is.
                Box(Modifier.fillMaxSize().background(RedvaultTheme.colors.bg), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator(color = RedvaultTheme.colors.accent)
                }
            } else {
                LaunchedEffect(session, conversation.id) {
                    lifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                        // A group asks which of its calls are going on, for the Join banner.
                        if (conversation.isGroup) launch {
                            while (true) {
                                session.groupCalls.refreshActive()
                                kotlinx.coroutines.delay(15_000)
                            }
                        }
                        session.chats.pollThread(conversation.id)
                    }
                }
                // One chat's message box, reply and selection never carry over into another chat's.
                key(conversation.id) {
                ThreadScreen(
                    conversation = conversation,
                    error = chats.sendError ?: chats.error,
                    onUseOlderProtocol = if (chats.sendErrorOlder && chats.sendErrorChat == conversation.id) ({ session.chats.useOlderProtocol(conversation.id) }) else null,
                    deviceKeysNote = deviceKeysNote(chats.v3Protocols[conversation.id], chats.v3Held),
                    onBack = ::pop,
                    onSend = { text, replyTo, onResult -> session.chats.send(conversation.id, text, replyTo, onResult) },
                    onDelete = { session.chats.delete(conversation.id, it) },
                    onDeleteForMe = { session.chats.deleteForMe(conversation.id, it) },
                    onToggleReaction = { message, emoji -> session.chats.toggleReaction(conversation.id, message, emoji) },
                    onSetDisappearing = { session.chats.setDisappearing(conversation.id, it) },
                    onRequestVerification = { session.chats.requestVerification(conversation.id, it) },
                    onOpenVerify = { push(Route.Verify(conversation.id)) },
                    onOpenGroupInfo = { push(Route.GroupInfo(conversation.id)) },
                    onOpenDetails = { push(Route.MessageDetails(conversation.id, it)) },
                    onReport = { reason -> session.account.report(conversation.meta.otherUserId, reason) },
                    onSendFiles = { files, onStage, onResult -> session.chats.sendPendingBatch(conversation.id, files, onStage, onResult) },
                    onSendVoice = { file, onStage, onResult -> session.chats.sendVoice(conversation.id, file, onStage, onResult) },
                    onDownloadFile = { descriptor, onStage -> session.chats.downloadFile(descriptor, onStage) },
                    onRetryQueued = session.chats::retryQueued,
                    onDiscardQueued = session.chats::discardQueued,
                    torNotice = when {
                        torStatus == null -> null
                        torStatus.state == TorState.Unavailable -> TOR_UNAVAILABLE_MESSAGE
                        chats.queuedHeld > 0 -> "${chats.queuedHeld} not sent: the server refused ${if (chats.queuedHeld == 1) "it" else "them"}. Retry or discard below."
                        chats.queued > 0 && torStatus.state == TorState.Connecting -> "Connecting to Tor. ${chats.queued} waiting to send."
                        else -> null
                    },
                    canSendFiles = isPaidTier(account.profile.subscriptionTier),
                    blocked = account.blocked.any { it.userId == conversation.meta.otherUserId },
                    onSetBlocked = { block ->
                        if (block) session.account.block(conversation.meta.otherUserId)
                        else session.account.unblock(conversation.meta.otherUserId)
                    },
                    online = conversation.meta.otherUserId in live.online,
                    typing = live.typingIn(conversation.id).map { id ->
                        conversation.meta.members.firstOrNull { it.userId == id }?.handle ?: conversation.handle
                    },
                    onTyping = { session.live.notifyTyping(conversation.id) },
                    onCall = if (callsState.available && !conversation.isGroup) { kind -> call(conversation.id, kind) } else null,
                    // Not in communities on this app yet (see GroupCallManager.groupOf).
                    onGroupCall = if (groupState.available && conversation.isGroup && conversation.meta.community == null) { kind -> groupCallIn(conversation.id, kind) } else null,
                    ongoingGroupCall = groupState.active[conversation.id],
                    groupCallBlocked = !session.groupCalls.canStart && groupState.active[conversation.id] == null,
                    callBusy = groupCall != null && groupCall.phase != GroupPhase.Ended || oneToOneCall != null && oneToOneCall.phase != CallPhase.Ended,
                    inThisGroupCall = groupCall != null && groupCall.phase != GroupPhase.Ended && groupCall.conversationId == conversation.id,
                    webAppBase = session.webAppBase,
                    onJoinGroupCallLink = { id ->
                        askCallPermissions(false) { _ -> session.groupCalls.joinByLink(id) { problem -> problem?.let(::toast) } }
                    },
                    onOpenGroupInvite = { linkId, secret -> push(Route.JoinGroup(linkId, secret)) },
                    onOpenChannel = { link -> push(Route.Channel(link.id, link.secret.orEmpty())) },
                    onChannelPostShown = { postId -> session.channels.countViews(listOf(postId)) },
                    selfHandle = account.profile.handle,
                    // Not into a channel this user only reads.
                    forwardTargets = chats.conversations.filter { it.id != conversation.id && it.meta.community?.canPost != false },
                    onEdit = { messageId, text, onResult -> session.chats.edit(conversation.id, messageId, text, onResult) },
                    onForward = { messages, toIds, onResult -> session.chats.forwardMany(messages, toIds, onResult) },
                    onDeleteMany = { ids, forEveryone ->
                        if (forEveryone) session.chats.deleteManyForEveryone(conversation.id, ids) else session.chats.deleteManyForMe(conversation.id, ids)
                    },
                    onLoadOlder = { onDone -> session.chats.loadOlder(conversation.id, onDone) },
                    pictures = session.chats.pictures,
                    initialDraft = remember { session.chats.draftFor(conversation.id) },
                    onDraftChange = { session.chats.setDraft(conversation.id, it) },
                    incomingShare = shareInto?.takeIf { it.first == conversation.id }?.second,
                    onShareTaken = { if (shareInto?.first == conversation.id) shareInto = null },
                    // A 1:1 chat's details are its contact's page (shared media, mute, favorite, timer).
                    onOpenChatDetails = if (!conversation.isGroup && conversation.meta.otherUserId.isNotEmpty()) {
                        { push(Route.Contact(conversation.meta.otherUserId)) }
                    } else {
                        null
                    },
                    onOpenSharedMedia = { push(Route.SharedMedia(conversation.id)) },
                    onTogglePin = { session.chats.togglePin(conversation.id, it) },
                    onUnpin = { session.chats.unpin(conversation.id, it) },
                    onCreatePoll = { question, options, multiple, onResult ->
                        session.chats.createPoll(conversation.id, question, options, multiple, onResult)
                    },
                    onVote = { poll, options -> session.chats.vote(conversation.id, poll, options) },
                    onEndPoll = { session.chats.endPoll(conversation.id, it) },
                    onOpenCommunity = conversation.meta.community?.let { community -> { push(Route.Community(community.id)) } },
                    onReportCommunityMessage = conversation.meta.community?.let { community ->
                        { message, category, details, shareText ->
                            session.communities.reportMessage(
                                community.id, message.id, message.senderId.ifEmpty { null }, message.text, category, details, shareText,
                            )
                        }
                    },
                )
                }
            }
        }

        Route.ShareTo -> ShareToScreen(
            items = pendingShare,
            // Not into a channel this user only reads.
            conversations = chats.conversations.filter { it.meta.community?.canPost != false },
            onPick = { id ->
                pendingShare?.let { shareInto = id to it }
                pendingShare = null
                // Into that chat, on top: a copy of it further down the stack (it was open, or opened by an
                // earlier share) goes, so Back doesn't land in the same chat twice.
                val rest = stack.dropLast(1).filterNot { (it as? Route.Thread)?.conversationId == id }
                encodedStack = (rest + Route.Thread(id)).joinToString("\n") { it.encode() }
            },
            onBack = {
                pendingShare = null
                pop()
            },
        )

        is Route.Communities -> CommunitiesScreen(
            session = session.communities,
            page = CommunitiesPage.of(route.page),
            onBack = ::pop,
            onPage = { replaceTop(Route.Communities(it.key)) },
            onOpenCommunity = { push(Route.Community(it)) },
        )
        is Route.Community -> CommunityScreen(
            session = session.communities,
            communityId = route.id,
            onBack = ::pop,
            onOpenChat = { conversationId ->
                // Usually loaded already; if it was just enrolled, the spinner shows until the list catches up.
                scope.launch { session.chats.refreshNow() }
                push(Route.Thread(conversationId))
            },
            onMembershipChanged = { scope.launch { session.chats.refreshNow() } },
        )

        is Route.Contact -> {
            val conversation = contactsOf(chats.conversations).firstOrNull { it.meta.otherUserId == route.userId }
            ContactScreen(
                conversation = conversation,
                allConversations = chats.conversations,
                live = live,
                blocked = account.blocked.any { it.userId == route.userId },
                onBack = ::pop,
                onMessage = {
                    conversation?.let { c ->
                        // Opened from that chat (Chat details): back to it, rather than a second copy on the stack.
                        if ((stack.getOrNull(stack.size - 2) as? Route.Thread)?.conversationId == c.id) pop() else push(Route.Thread(c.id))
                    }
                },
                onVerify = { conversation?.let { push(Route.Verify(it.id)) } },
                onToggleFavorite = { session.live.toggleFavorite(route.userId) },
                onToggleMuted = { session.live.toggleMuted(route.userId) },
                onSetDisappearing = { d -> conversation?.let { session.chats.setDisappearing(it.id, d) } },
                onSetBlocked = { block -> if (block) session.account.block(route.userId) else session.account.unblock(route.userId) },
                onReport = { reason -> session.account.report(route.userId, reason) },
                onCall = if (callsState.available && conversation != null) { kind -> call(conversation.id, kind) } else null,
                onOpenSharedMedia = { conversation?.let { push(Route.SharedMedia(it.id)) } },
                extra = { StoryHighlightsRow(session.stories, stories, route.userId, onOpenList = { push(Route.StoryViewer("list")) }) },
            )
        }
        Route.ContactCode -> ContactCodeScreen(account.profile.handle, onBack = ::pop)
        is Route.SharedMedia -> SharedMediaScreen(
            conversation = chats.conversations.firstOrNull { it.id == route.conversationId },
            onBack = ::pop,
            onDownload = { descriptor, onStage -> session.chats.downloadFile(descriptor, onStage) },
        )
        Route.LinkedDevices -> LinkedDevicesScreen(session.security, onBack = ::pop)
        Route.DeviceKeys -> session.deviceKeys?.let { DeviceKeysScreen(it, onBack = ::pop) } ?: LaunchedEffect(Unit) { pop() }
        Route.RelayServers -> RelayServersScreen(session.security, onBack = ::pop)
        Route.NetworkRouting -> NetworkRoutingScreen(
            tor = session.tor,
            queued = chats.queued,
            strictNoPush = strictNoPush,
            onStrictNoPushChange = { on ->
                session.push.strictNoPush = on
                strictNoPush = on
                notificationsOn = (!on || session.push.torRouted) && session.push.enabled
                // On: the endpoint goes from our server and the distributor, and the background connection stops;
                // with Tor routing it then starts again (it only ever reaches our onion service, through Tor).
                // Off: notifications come back if they were on.
                if (on) {
                    val host = activity
                    scope.launch {
                        session.push.unregister()
                        if (session.push.torRouted && host != null) session.push.register(host)
                    }
                } else {
                    registerPush()
                }
            },
            onBack = ::pop,
        )
        Route.TransparencyLog -> TransparencyLogScreen(session.security, onBack = ::pop, onOpenFeed = { push(Route.Transparency) })
        is Route.StoryViewer -> {
            // The people to go through are fixed when the viewer opens, so a refresh (and rings turning grey) can't reshuffle them.
            val groups = remember(route.authorId) {
                if (route.authorId == "list") {
                    session.stories.takeList()
                } else if (route.authorId == "me") {
                    listOf(ViewerGroup(session.identity.userId, account.profile.handle, account.profile.avatarUrl, stories.own, isOwn = true)).filter { it.stories.isNotEmpty() }
                } else {
                    val from = stories.authors.indexOfFirst { it.author.userId == route.authorId }.coerceAtLeast(0)
                    stories.authors.drop(from).map { ViewerGroup(it.author.userId, it.author.handle, it.author.avatarUrl, it.stories, isOwn = false) }
                }
            }
            StoryViewerScreen(session.stories, groups, onClose = ::pop, onCompose = { replaceTop(Route.StoryComposer) })
        }

        Route.StoryComposer -> StoryComposerScreen(session.stories, stories, onClose = ::pop)

        Route.StorySettings -> StorySettingsScreen(session.stories, stories, onBack = ::pop, onOpenList = { push(Route.StoryViewer("list")) })

        is Route.AddFriendAs -> AddFriendScreen(
            account = session.account,
            onBack = ::pop,
            onOpenRequests = { push(Route.FriendRequests) },
            initialHandle = route.handle,
        )

        Route.AddFriend -> AddFriendScreen(
            account = session.account,
            onBack = ::pop,
            onOpenRequests = { push(Route.FriendRequests) },
        )

        Route.FriendRequests -> FriendRequestsScreen(
            account = session.account,
            requests = account.friendRequests,
            onBack = ::pop,
            onAddFriend = { push(Route.AddFriend) },
            onChatCreated = { resetTo(Route.Thread(it)) },
        )

        Route.MessageRequests -> MessageRequestsScreen(
            requests = chats.requests,
            onRespond = session.chats::respondToRequest,
            onBlock = session.account::block,
            onOpenChat = { resetTo(Route.Thread(it)) },
            onBack = ::pop,
        )

        Route.Settings -> SettingsScreen(
            account = session.account,
            profile = account.profile,
            incomingRequests = incoming,
            readReceipts = chats.readReceipts,
            onReadReceiptsChange = session.chats::setReadReceipts,
            typingIndicators = live.typingIndicators,
            onTypingIndicatorsChange = session.live::setTypingIndicators,
            onOpenLinkedDevices = { push(Route.LinkedDevices) },
            onOpenDeviceKeys = if (session.deviceKeys != null) ({ push(Route.DeviceKeys) }) else null,
            onOpenRelayServers = { push(Route.RelayServers) },
            onOpenNetworkRouting = { push(Route.NetworkRouting) },
            torRouting = session.tor != null,
            onOpenTransparencyLog = { push(Route.TransparencyLog) },
            ringtone = ringtone,
            onRingtoneChange = { on -> session.settings.ringtone = on; ringtone = on },
            echoCancellation = echoCancellation,
            onEchoCancellationChange = { on -> session.settings.echoCancellation = on; echoCancellation = on },
            onOpenStorySettings = if (stories.on) { { push(Route.StorySettings) } } else null,
            hideContent = hideContent,
            onHideContentChange = {
                session.settings.hideContent = it
                hideContent = it
            },
            notificationsOn = notificationsOn,
            onNotificationsChange = { on ->
                if (on && strictNoPush) return@SettingsScreen
                session.push.enabled = on
                notificationsOn = on
                // On: the permission answer registers (see askPermission); before Android 13 there's nothing to ask.
                if (!on) scope.launch { session.push.unregister() }
                else if (Build.VERSION.SDK_INT >= 33) askPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
                else registerPush()
            },
            pushMode = pushMode,
            distributorName = session.push::distributorName,
            distributorCount = remember(pushMode, foregrounds) { session.push.distributors().size },
            backgroundConnection = backgroundConnection,
            onBackgroundConnectionChange = { on ->
                backgroundConnection = on
                activity?.let { host -> scope.launch { session.push.setBackgroundConnection(on, host) } }
            },
            onPickDistributor = { activity?.let(session.push::pickDistributor) },
            onBack = ::pop,
            onOpenFriendRequests = { push(Route.FriendRequests) },
            onOpenChangeUsername = { push(Route.ChangeUsername) },
            onOpenChangePassword = { push(Route.ChangePassword) },
            onOpenMfa = { push(Route.Mfa) },
            onOpenTransparency = { push(Route.Transparency) },
            appLockEnabled = session.lock.store.enabled,
            appLockTimeout = session.lock.store.timeout,
            onOpenAppLock = { push(Route.AppLockSettings) },
            onOpenPrivacyMode = { push(Route.PrivacyMode) },
            blockedCount = account.blocked.size,
            onOpenBlocked = { push(Route.Blocked) },
            inboxUnread = inboxState.unread,
            onOpenInbox = { push(Route.Inbox) },
            onOpenRecoveryKey = { push(Route.RecoveryKeySettings) },
            onOpenDeleteAccount = { push(Route.DeleteAccount) },
            onLogout = onLogout,
        )

        Route.ChangeUsername -> ChangeUsernameScreen(session.account, current = account.profile.handle, onBack = ::pop)
        Route.ChangePassword -> ChangePasswordScreen(session.account, onBack = ::pop)
        Route.Mfa -> MfaScreen(session.account, enabled = account.profile.mfaEnabled, onBack = ::pop)
        Route.DeleteAccount -> DeleteAccountScreen(session.account, onBack = ::pop, onDeleted = onLogout)

        is Route.Verify -> {
            val conversation = chats.conversations.firstOrNull { it.id == route.conversationId }
            if (conversation == null) {
                LaunchedEffect(Unit) { pop() }
            } else {
                val deviceKeySafety = remember(conversation.id) { DeviceKeySafetyState() }
                VerifyScreen(
                    conversation = conversation,
                    onBack = ::pop,
                    onSetVerified = { session.chats.setContactVerified(conversation.id, it) },
                    top = { DeviceKeyChangedNotice(deviceKeySafety) },
                    extra = {
                        val keys = session.deviceKeys
                        if (keys != null && !conversation.isGroup) {
                            DeviceKeySafetyNumberSection(keys, conversation.meta.otherUserId, conversation.id, deviceKeySafety)
                        }
                    },
                )
            }
        }

        is Route.MessageDetails -> MessageDetailsScreen(
            message = chats.conversations.firstOrNull { it.id == route.conversationId }
                ?.messages?.firstOrNull { it.id == route.messageId },
            onBack = ::pop,
        )

        Route.Transparency -> TransparencyFeedScreen(loadFeed = loadTransparencyFeed, onBack = ::pop)

        Route.AppLockSettings -> AppLockSettingsScreen(session.lock, onBack = ::pop)

        Route.NewGroup -> NewGroupScreen(session.chats, onBack = ::pop, onCreated = { resetTo(Route.Thread(it)) })

        is Route.GroupInfo -> {
            val group = chats.conversations.firstOrNull { it.id == route.conversationId && it.isGroup }
            val community = group?.meta?.community
            if (group == null) {
                LaunchedEffect(Unit) { pop() }
            } else if (community != null) {
                // A community is managed from its own page, never the generic group screens.
                LaunchedEffect(Unit) { replaceTop(Route.Community(community.id)) }
            } else {
                GroupInfoScreen(group, session.chats, onBack = ::pop, onLeft = { encodedStack = Route.Home().encode() }, onOpenSharedMedia = { push(Route.SharedMedia(group.id)) })
            }
        }

        is Route.JoinGroup -> JoinGroupScreen(
            linkId = route.linkId,
            secret = route.secret,
            chats = session.chats,
            onBack = ::pop,
            onOpenGroup = { resetTo(Route.Thread(it)) },
        )

        is Route.Channel -> ChannelScreen(
            channelId = route.id,
            linkSecret = route.secret.ifEmpty { null },
            channels = session.channels,
            webAppBase = session.webAppBase,
            forwardTargets = chats.conversations,
            onForward = session.chats::forwardChannelPost,
            onOpenChannel = { link ->
                if (link.id != route.id || (link.secret != null && link.secret != route.secret)) push(Route.Channel(link.id, link.secret.orEmpty()))
            },
            onBack = ::pop,
        )

        Route.RecoveryKeySettings -> RecoveryKeySettingsScreen(session.account, account.profile, onBack = ::pop)

        Route.Inbox -> InboxScreen(session.inbox, onBack = ::pop)

        Route.Blocked -> BlockedUsersScreen(
            blocked = account.blocked,
            onUnblock = { session.account.unblock(it) },
            onBack = ::pop,
        )

        is Route.FileDetail -> FileDetailScreen(
            files = session.files,
            state = filesState,
            id = route.id,
            contacts = shareContacts(chats),
            onBack = ::pop,
            onUnlock = { then -> push(Route.UnlockFile(route.id, then)) },
            onChangeLock = { push(Route.ChangeLock(route.id, "")) },
            onAddLock = { push(Route.ChangeLock(route.id, "")) },
            onShare = { push(Route.ShareFile(route.id)) },
        )
        is Route.ShareFile -> ShareFileScreen(
            session.files, filesState, route.id, shareContacts(chats), onBack = ::pop, onPublicLink = { push(Route.ShareLink(route.id)) },
        )
        is Route.ShareLink -> ShareLinkScreen(
            session.shares, sharesState, filesState.find(route.id), onBack = ::pop, onManage = { push(Route.SharedLinks) },
        )
        Route.SharedLinks -> SharedLinksScreen(
            session.shares, sharesState, filesState, onBack = ::pop, onEdit = { id, mode -> push(Route.ShareLinkEdit(id, mode)) },
        )
        is Route.ShareLinkEdit -> ShareLinkEditScreen(session.shares, sharesState, route.id, route.mode, onBack = ::pop)
        Route.Incoming -> IncomingScreen(session.shares, sharesState, filesState, onBack = ::pop)
        Route.UploadLock -> UploadLockSheet(
            files = session.files,
            state = filesState,
            uris = pendingUploads.first,
            parent = pendingUploads.second,
            changeId = null,
            startMode = null,
            onDone = {
                pendingUploads = emptyList<android.net.Uri>() to ""
                pop()
            },
        )
        is Route.ChangeLock -> UploadLockSheet(
            files = session.files,
            state = filesState,
            uris = emptyList(),
            parent = "",
            changeId = route.id,
            startMode = route.mode.ifEmpty { null },
            onDone = ::pop,
        )
        is Route.UnlockFile -> UnlockFileSheet(session.files, filesState, route.id) { unlocked ->
            when {
                unlocked && route.then == "changeLock" -> replaceTop(Route.ChangeLock(route.id, ""))
                unlocked && route.then == "removeLock" -> replaceTop(Route.ChangeLock(route.id, "none"))
                else -> pop()
            }
        }
        Route.FileKeys -> FileKeysScreen(
            state = filesState,
            onBack = ::pop,
            onCreate = { push(Route.KeyCreate) },
            onImport = { push(Route.KeyImport) },
            onOpen = { push(Route.FileKey(it)) },
        )
        is Route.FileKey -> FileKeyScreen(session.files, filesState, route.id, onBack = ::pop, onPassword = { action -> push(Route.KeyPassword(route.id, action)) })
        Route.KeyCreate -> KeyCreateSheet(session.files, filesState, onDone = ::pop)
        Route.KeyImport -> KeyImportSheet(session.files, filesState, onDone = ::pop)
        is Route.KeyPassword -> KeyPasswordSheet(session.files, filesState, route.id, route.action, onDone = ::pop)

        is Route.PasswordItem -> PasswordItemScreen(
            vault = session.vault,
            state = vaultState,
            id = route.id,
            contacts = shareContacts(chats),
            onBack = ::pop,
            onEdit = { push(Route.PasswordEdit(route.id, "")) },
            onShare = { push(Route.PasswordShare(route.id)) },
        )
        is Route.PasswordEdit -> PasswordEditSheet(session.vault, vaultState, route.id.ifEmpty { null }, route.type.ifEmpty { "login" }) { savedId ->
            when {
                // A new item opens once saved; a deleted one goes back to the list.
                route.id.isEmpty() && !savedId.isNullOrEmpty() -> replaceTop(Route.PasswordItem(savedId))
                savedId == "" -> selectTab(Tabs.PASSWORDS)
                else -> pop()
            }
        }
        is Route.PasswordShare -> PasswordShareSheet(session.vault, vaultState, route.id, shareContacts(chats), onDone = ::pop)

        Route.PrivacyMode -> PrivacyModeScreen(
            readReceipts = chats.readReceipts,
            hideContent = hideContent,
            appLockOn = session.lock.store.enabled,
            onEnable = {
                session.chats.setReadReceipts(false)
                session.settings.hideContent = true
                hideContent = true
                session.live.setTypingIndicators(false)
                if (account.profile.showOnlineStatus == true) scope.launch { runCatching { session.account.setShowOnlineStatus(false) } }
            },
            onSetUpAppLock = { push(Route.AppLockSettings) },
            onBack = ::pop,
            typingIndicators = live.typingIndicators,
            showOnlineStatus = account.profile.showOnlineStatus,
        )
    }
    }
    }
}

/** People you can share a file with: everyone you have a one-to-one chat with. */
private fun shareContacts(chats: ChatsState): List<ShareContact> =
    chats.conversations.filter { !it.isGroup }.map { ShareContact(it.meta.otherUserId, it.handle, it.avatarUrl) }.sortedBy { it.handle.lowercase() }

/** What the thread screen says about device keys (M5): the chat's protocol, and messages waiting on the server. */
internal fun deviceKeysNote(protocol: String?, held: Int): String? {
    val parts = buildList {
        when (protocol) {
            "signal" -> add("Signal sessions (development build), with post-quantum key agreement, to every device in this chat.")
            "mixed" -> add("Some devices in this chat use Signal sessions, others the device-key protocol.")
        }
        if (held > 0) {
            add((if (held == 1) "A device-key message is" else "$held device-key messages are") + " waiting: a contact's keys changed, or a group key hasn't arrived yet. Compare safety numbers (Verify) if a contact's keys changed.")
        }
    }
    return parts.takeIf { it.isNotEmpty() }?.joinToString(" ")
}
