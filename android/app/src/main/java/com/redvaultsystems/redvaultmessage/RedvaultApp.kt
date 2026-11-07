package com.redvaultsystems.redvaultmessage

import com.redvaultsystems.redvaultmessage.devicekeys.V3Chat
import com.redvaultsystems.redvaultmessage.devicekeys.AndroidKeystoreWrapper
import com.redvaultsystems.redvaultmessage.devicekeys.DeviceKeysSession
import com.redvaultsystems.redvaultmessage.devicekeys.DeviceStorage
import com.redvaultsystems.redvaultmessage.devicekeys.EngineRunner
import com.redvaultsystems.redvaultmessage.devicekeys.NativeEngine
import com.redvaultsystems.redvaultmessage.devicekeys.RetrofitV3Api
import com.redvaultsystems.redvaultmessage.data.SealedRecords
import com.redvaultsystems.redvaultmessage.crypto.ShareCrypto
import com.redvaultsystems.redvaultmessage.data.ShareRepository
import com.redvaultsystems.redvaultmessage.ui.files.SharesSession
import android.app.Application
import com.goterl.lazysodium.LazySodiumAndroid
import com.goterl.lazysodium.SodiumAndroid
import com.redvaultsystems.redvaultmessage.crypto.CallCrypto
import com.redvaultsystems.redvaultmessage.crypto.ChannelCrypto
import com.redvaultsystems.redvaultmessage.crypto.CommunityCrypto
import com.redvaultsystems.redvaultmessage.crypto.DriveCrypto
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.FileCrypto
import com.redvaultsystems.redvaultmessage.crypto.GroupCallCrypto
import com.redvaultsystems.redvaultmessage.crypto.GroupInviteCrypto
import com.redvaultsystems.redvaultmessage.crypto.RecoveryKey
import com.redvaultsystems.redvaultmessage.crypto.StoryAnswerCrypto
import com.redvaultsystems.redvaultmessage.crypto.StoryCrypto
import com.redvaultsystems.redvaultmessage.crypto.VaultCrypto
import com.redvaultsystems.redvaultmessage.data.AccountRepository
import com.redvaultsystems.redvaultmessage.data.AppLockStore
import com.redvaultsystems.redvaultmessage.data.AuthRepository
import com.redvaultsystems.redvaultmessage.data.ChannelDeviceStore
import com.redvaultsystems.redvaultmessage.data.ChannelMediaClient
import com.redvaultsystems.redvaultmessage.data.ChannelRepository
import com.redvaultsystems.redvaultmessage.data.ChatSettingsStore
import com.redvaultsystems.redvaultmessage.data.CallHistoryStore
import com.redvaultsystems.redvaultmessage.data.CommunityKeys
import com.redvaultsystems.redvaultmessage.data.ContactKeyPinPrefs
import com.redvaultsystems.redvaultmessage.data.ContactKeyPins
import com.redvaultsystems.redvaultmessage.data.ContactPrefsStore
import com.redvaultsystems.redvaultmessage.data.ConversationRepository
import com.redvaultsystems.redvaultmessage.data.DriveRepository
import com.redvaultsystems.redvaultmessage.data.FileKeyRepository
import com.redvaultsystems.redvaultmessage.data.FileLocks
import com.redvaultsystems.redvaultmessage.data.MessageKeys
import com.redvaultsystems.redvaultmessage.data.KeyPinStore
import com.redvaultsystems.redvaultmessage.data.KeyTrust
import com.redvaultsystems.redvaultmessage.data.SigningKeyPinStore
import com.redvaultsystems.redvaultmessage.data.FollowedChannelsStore
import com.redvaultsystems.redvaultmessage.data.VaultRepository
import com.redvaultsystems.redvaultmessage.data.VaultRevStore
import com.redvaultsystems.redvaultmessage.data.FileRepository
import com.redvaultsystems.redvaultmessage.data.FriendRepository
import com.redvaultsystems.redvaultmessage.data.GroupInviteRepository
import com.redvaultsystems.redvaultmessage.data.GroupRepository
import com.redvaultsystems.redvaultmessage.data.HiddenMessagesStore
import com.redvaultsystems.redvaultmessage.data.StoriesRepository
import com.redvaultsystems.redvaultmessage.data.StoryDraftStore
import com.redvaultsystems.redvaultmessage.data.StoryInteractions
import com.redvaultsystems.redvaultmessage.data.StorySeenStore
import com.redvaultsystems.redvaultmessage.data.VerifiedContactsStore
import com.redvaultsystems.redvaultmessage.data.Outbox
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.tor.TorController
import okhttp3.HttpUrl.Companion.toHttpUrl
import java.io.File
import com.redvaultsystems.redvaultmessage.data.api.SessionCookieJar
import com.redvaultsystems.redvaultmessage.data.api.TransparencyEventDto
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.PreferenceSignInPins
import com.redvaultsystems.redvaultmessage.data.SignInKeys
import com.redvaultsystems.redvaultmessage.identity.LastHandleStore
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import com.redvaultsystems.redvaultmessage.push.Notifications
import com.redvaultsystems.redvaultmessage.push.PushFallbackService
import com.redvaultsystems.redvaultmessage.push.PushRegistrar
import com.redvaultsystems.redvaultmessage.ui.SignedInSession
import com.redvaultsystems.redvaultmessage.ui.account.AccountSession
import com.redvaultsystems.redvaultmessage.ui.channels.ChannelNotifications
import com.redvaultsystems.redvaultmessage.ui.channels.ChannelSession
import com.redvaultsystems.redvaultmessage.ui.chats.ChatSession
import com.redvaultsystems.redvaultmessage.ui.communities.CommunitySession
import com.redvaultsystems.redvaultmessage.ui.inbox.InboxSession
import com.redvaultsystems.redvaultmessage.ui.files.FilesSession
import com.redvaultsystems.redvaultmessage.ui.components.SecretClipboard
import com.redvaultsystems.redvaultmessage.calls.CallManager
import com.redvaultsystems.redvaultmessage.calls.CallNotifications
import com.redvaultsystems.redvaultmessage.calls.CallPhase
import com.redvaultsystems.redvaultmessage.calls.GroupCallManager
import com.redvaultsystems.redvaultmessage.ui.live.LiveSession
import com.redvaultsystems.redvaultmessage.ui.stories.StoriesSession
import com.redvaultsystems.redvaultmessage.ui.settings.SecuritySession
import com.redvaultsystems.redvaultmessage.ui.passwords.VaultSession
import com.redvaultsystems.redvaultmessage.ui.components.AvatarLoader
import com.redvaultsystems.redvaultmessage.ui.lock.AppLock

class RedvaultApp : Application() {
    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        container = AppContainer(this)
        Notifications.createChannel(this)
        PushFallbackService.createChannel(this)
        CallNotifications.createChannels(this)
        ChannelNotifications.createChannel(this)
        // Plaintext from a previous run (e.g. the app was killed mid-session) never outlives it.
        container.clearFileCache()
    }
}

/** Hand-wired dependencies: small enough that a DI framework isn't worth it yet. */
class AppContainer(app: Application) {
    private val sodium = LazySodiumAndroid(SodiumAndroid())
    val crypto = E2eCrypto(sodium)
    private val cookieJar = SessionCookieJar()

    /**
     * Tor routing (docs/tor-routing-plan.md): with an onion API base, text, photos, files, Stories
     * and voice messages leave only through the embedded Tor gateway, never directly. Null for a
     * clearnet server. Calls are WebRTC through the TURN relay either way, never Tor.
     */
    val tor: TorController? = BuildConfig.API_BASE.toHttpUrl().host.takeIf(TorController::isOnionHost)?.let { host ->
        // noBackupFilesDir: Tor's guard state and directory cache never go into a device backup.
        TorController(host, File(app.noBackupFilesDir, "tor")).also { it.start() }
    }
    private val httpClient = ApiClient.httpClient(cookieJar, BuildConfig.API_BASE, tor)
    private val api = ApiClient.create(BuildConfig.API_BASE, httpClient)
    private val driveApi = ApiClient.createDrive(BuildConfig.API_BASE, httpClient)
    private val shareApi = ApiClient.createShares(BuildConfig.API_BASE, httpClient)
    private val liveApi = ApiClient.createLive(BuildConfig.API_BASE, httpClient)
    private val callApi = ApiClient.createCalls(BuildConfig.API_BASE, httpClient)
    private val securityApi = ApiClient.createSecurity(BuildConfig.API_BASE, httpClient)
    private val groupInviteApi = ApiClient.createGroupInvites(BuildConfig.API_BASE, httpClient)
    private val channelApi = ApiClient.createChannels(BuildConfig.API_BASE, httpClient)
    // Reading channels (and their photos and media) goes out without the session cookie (see ApiClient.publicClient).
    private val publicClient = ApiClient.publicClient(BuildConfig.API_BASE, tor)
    private val publicChannelApi = ApiClient.createPublicChannels(BuildConfig.API_BASE, publicClient)
    private val callCrypto = CallCrypto(sodium, crypto)
    private val groupCallCrypto = GroupCallCrypto(sodium, crypto)
    private val callHistory by lazy { CallHistoryStore(appContext, crypto, callCrypto) }
    /** The web app's address (call links open there): the API base without its "api/". */
    private val webAppBase = BuildConfig.API_BASE.removeSuffix("/").removeSuffix("api").let { if (it.endsWith("/")) it else "$it/" }
    private val driveCrypto = DriveCrypto(sodium, crypto)
    private val vaultCrypto = VaultCrypto(sodium, crypto, driveCrypto)
    private val fileLocks = FileLocks(driveCrypto, vaultCrypto)
    private val fileCrypto = FileCrypto(sodium)
    private val shareCrypto = ShareCrypto(sodium)
    private val appContext = app
    private val lastHandleStore = LastHandleStore(app)
    private val recoveryKeys = RecoveryKey(sodium, crypto)
    private val hiddenMessages = HiddenMessagesStore(app)
    /** Group key epochs and message signatures, for whoever is signed in (docs/group-keys-and-signatures.md). */
    private val contactKeyPins = ContactKeyPins(ContactKeyPinPrefs(app))
    private val messageKeys = MessageKeys(api, crypto, SigningKeyPinStore(app), KeyTrust(crypto, KeyPinStore(app), contactKeyPins))
    private val communityApi = ApiClient.createCommunities(BuildConfig.API_BASE, httpClient)
    /** Community epoch keys, in memory only; set to the signed-in identity per session and forgotten on sign-out. */
    private val communityKeys = CommunityKeys(communityApi, CommunityCrypto(crypto))
    private val conversationRepository = ConversationRepository(api, crypto, hiddenMessages, messageKeys, communityKeys)
    private val fileRepository = FileRepository(
        api, conversationRepository, fileCrypto, httpClient, BuildConfig.API_BASE, app.contentResolver, app.cacheDir,
    )
    private val friendRepository = FriendRepository(api, crypto, contactKeyPins)
    private val storyInteractions = StoryInteractions(ApiClient.createStoryInteractions(BuildConfig.API_BASE, httpClient), StoryAnswerCrypto(sodium))
    private val storiesRepository = StoriesRepository(
        ApiClient.createStories(BuildConfig.API_BASE, httpClient), api, StoryCrypto(sodium), fileCrypto, httpClient, BuildConfig.API_BASE,
        app.cacheDir, StorySeenStore(app),
    )
    private val storyDrafts = StoryDraftStore(java.io.File(app.filesDir, "story-drafts"), java.io.File(app.cacheDir, "stories/drafts-work"), StoryCrypto(sodium), fileCrypto)
    private val groupRepository = GroupRepository(api, crypto, messageKeys)
    private val groupInviteRepository = GroupInviteRepository(groupInviteApi, api, GroupInviteCrypto(sodium, crypto), crypto, webAppBase, messageKeys)
    private val channelRepository = ChannelRepository(publicChannelApi, channelApi)
    private val followedChannels = FollowedChannelsStore(app)
    private val channelDevice = ChannelDeviceStore(app)
    private val channelCrypto = ChannelCrypto(sodium, crypto)
    private val channelMedia = ChannelMediaClient(publicClient, httpClient, BuildConfig.API_BASE, app.contentResolver, app.cacheDir)
    private val signInKeys = SignInKeys(api, crypto, PreferenceSignInPins(app))
    private val accountRepository = AccountRepository(api, crypto, app.contentResolver, recoveryKeys, signInKeys)
    private val verifiedContacts = VerifiedContactsStore(app)
    private val contactPrefs = ContactPrefsStore(app)

    val chatSettings = ChatSettingsStore(app)
    private val appLockStore = AppLockStore(app, crypto)
    val avatarLoader = AvatarLoader(httpClient, BuildConfig.API_BASE)
    /** Push through UnifiedPush, or the app's own background connection (push/PushRegistrar.kt). Same client: Tor routing applies. */
    val pushRegistrar = PushRegistrar(app, ApiClient.createPush(BuildConfig.API_BASE, httpClient), torBuild = tor != null, signedIn = { hasActiveSession })
    val authRepository = AuthRepository(api, crypto, cookieJar, lastHandleStore, pushRegistrar, recoveryKeys, signInKeys)

    /** A channel to open, from a tapped new-post notification (MainActivity sets it; the signed-in app takes it). */
    val openChannelRequests = kotlinx.coroutines.flow.MutableStateFlow<String?>(null)

    /** A tapped "new message" notification (MainActivity sets it; the signed-in app picks the chat). */
    val openChatRequests = kotlinx.coroutines.flow.MutableStateFlow(false)

    /** Files or text shared from another app (ShareActivity -> MainActivity), until a chat is picked. Memory only. */
    val shareRequests = kotlinx.coroutines.flow.MutableStateFlow<com.redvaultsystems.redvaultmessage.ui.chats.SharedItems?>(null)

    /** True while someone is signed in in this process; lets a new push endpoint be registered at once, and keeps the background connection going. */
    @Volatile
    var hasActiveSession = false
        private set

    fun clearFileCache() {
        fileRepository.clearCache()
        channelMedia.clear()
        // Files: decrypted copies and leftover uploads from a session that never closed.
        java.io.File(appContext.cacheDir, "drive").deleteRecursively()
        // Stories: decrypted videos and unfinished uploads likewise.
        storiesRepository.clearCache()
    }

    /** The public transparency feed; works signed in or out. */
    suspend fun transparencyFeed(limit: Int): List<TransparencyEventDto> =
        apiCall(ApiClient.json) { api.transparencyFeed(limit) }

    fun createSession(identity: SelfIdentity, onSessionExpired: () -> Unit): SignedInSession {
        messageKeys.begin(identity)
        communityKeys.setIdentity(identity)
        // Tor routing: every message, photo, file and voice message waits sealed in this outbox until the
        // server has it (data/Outbox.kt). Per account; outside device backups.
        val outbox = tor?.let {
            Outbox(File(appContext.noBackupFilesDir, "outbox/${identity.userId}"), Outbox.deriveKey(sodium, identity.privateKey), sodium, fileCrypto)
        }
        val communities = CommunitySession(communityApi, communityKeys, onSessionExpired)
        // Per-device keys (protocol 3): development only, in builds made with -PredvaultE2eV3=true.
        val deviceKeys = if (BuildConfig.E2E_V3) {
            DeviceKeysSession(
                userId = identity.userId,
                legacyPrivateKey = { identity.privateKey.copyOf() },
                // noBackupFilesDir: the device's keys and state never go into a device backup.
                storage = DeviceStorage(File(appContext.noBackupFilesDir, "e2eV3"), AndroidKeystoreWrapper()),
                engine = NativeEngine.call()?.let(::EngineRunner),
                api = RetrofitV3Api(ApiClient.createV3(BuildConfig.API_BASE, httpClient)),
                onSessionExpired = onSessionExpired,
            )
        } else null
        val v3Chat = deviceKeys?.let { V3Chat(it, identity.userId, File(appContext.noBackupFilesDir, "e2eV3")) }
        val chats = ChatSession(
            identity, conversationRepository, fileRepository, groupRepository, groupInviteRepository, chatSettings, verifiedContacts, contactKeyPins,
            hiddenMessages, onSessionExpired, outbox = outbox, tor = tor, v3 = v3Chat,
        )
        val channels = ChannelSession(
            channelRepository, followedChannels, channelDevice, channelCrypto, channelMedia, identity,
            // With app lock on, a notification never says which channel or what it posted.
            notify = { notice -> ChannelNotifications.show(appContext, notice, hideContent = appLockStore.enabled) },
            onSessionExpired = onSessionExpired,
        )
        val account = AccountSession(
            identity, friendRepository, accountRepository, lastHandleStore, fileRepository, onSessionExpired,
            onConversationsChanged = chats::refreshNow,
        )
        val vault = VaultRepository(driveApi, api, vaultCrypto, identity, VaultRevStore(appContext), contactKeyPins)
        val drive = DriveRepository(
            driveApi, api, driveCrypto, vaultCrypto, fileCrypto, fileLocks, identity, httpClient, BuildConfig.API_BASE,
            appContext.contentResolver, appContext.cacheDir, contactKeyPins,
        )
        // Tor routing: Files uploads go in parts with a sealed journal, and downloads resume (outside device backups).
        if (tor != null) {
            drive.enableResumable(
                File(appContext.noBackupFilesDir, "transfers/${identity.userId}"),
                SealedRecords(sodium, SealedRecords.deriveTransferKey(sodium, identity.privateKey)),
            )
        }
        val files = FilesSession(drive, FileKeyRepository(vault, driveCrypto, fileLocks), fileLocks, driveCrypto, vaultCrypto, onSessionExpired, tor)
        // Public share links: files and folders anyone can open in a browser, and what people upload through them.
        val shares = SharesSession(
            ShareRepository(shareApi, shareCrypto, driveCrypto, fileCrypto, identity, httpClient, BuildConfig.API_BASE, webAppBase),
            files,
            onSessionExpired,
        )
        val passwords = VaultSession(vault, vaultCrypto, crypto, identity, onSessionExpired)
        val live = LiveSession(liveApi, identity, contactPrefs, onSessionExpired)
        chats.listenForV3Wake(live.events)
        val stories = StoriesSession(
            identity, storiesRepository, storyInteractions, storyDrafts,
            sendReply = { authorId, text, ref, onResult -> chats.sendStoryReply(authorId, text, ref, onResult) },
            chatKeys = { chats.state.value.conversations.filter { !it.isGroup }.associate { it.meta.otherUserId to it.meta.otherPublicKeyB64 } },
            onSessionExpired = onSessionExpired,
        )
        // The two call managers know about each other: nobody is in two calls at once.
        var groupCallsRef: GroupCallManager? = null
        val calls = CallManager(
            context = appContext,
            api = callApi,
            lookupUser = { id -> apiCall(ApiClient.json) { api.getUser(id) } },
            crypto = callCrypto,
            history = callHistory,
            identity = identity,
            live = live,
            webAppBase = webAppBase,
            conversations = { chats.state.value.conversations },
            tier = { account.state.value.profile.subscriptionTier },
            ringtone = { chatSettings.ringtone },
            echoCancellation = { chatSettings.echoCancellation },
            groupCallBusy = { groupCallsRef?.isBusy() == true },
            onSessionExpired = onSessionExpired,
        )
        val groupCalls = GroupCallManager(
            context = appContext,
            api = callApi,
            crypto = groupCallCrypto,
            identity = identity,
            live = live,
            webAppBase = webAppBase,
            apiBase = BuildConfig.API_BASE,
            signalingClient = { host -> ApiClient.signalingClient(host) },
            conversations = { chats.state.value.conversations },
            tier = { account.state.value.profile.subscriptionTier },
            sendChatMessage = { conversationId, text, onResult -> chats.send(conversationId, text, null, onResult) },
            oneToOneBusy = { calls.state.value.call?.let { it.phase != CallPhase.Ended } == true },
            ringtone = { chatSettings.ringtone },
            echoCancellation = { chatSettings.echoCancellation },
            onSessionExpired = onSessionExpired,
        )
        groupCallsRef = groupCalls
        val security = SecuritySession(
            context = appContext,
            security = securityApi,
            api = api,
            calls = callApi,
            sodium = sodium,
            drive = driveCrypto,
            e2e = crypto,
            identity = identity,
            apiBase = BuildConfig.API_BASE,
            probeRelay = calls::probeRelay,
            conversations = { chats.state.value.conversations },
            onSessionExpired = onSessionExpired,
        )
        val inbox = InboxSession(api, onSessionExpired)
        hasActiveSession = true
        return SignedInSession(identity, chats, account, files, shares, passwords, live, stories, calls, groupCalls, security, channels, inbox, communities, chatSettings, pushRegistrar, AppLock(appLockStore), avatarLoader, webAppBase, tor, deviceKeys) {
            hasActiveSession = false
            pushRegistrar.sessionEnded()
            messageKeys.end()
            // A password or key copied in the last 30 seconds doesn't outlive the session.
            SecretClipboard.clearNow(appContext)
        }
    }
}
