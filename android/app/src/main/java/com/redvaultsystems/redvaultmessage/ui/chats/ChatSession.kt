// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui.chats

import com.redvaultsystems.redvaultmessage.devicekeys.ChatOp
import com.redvaultsystems.redvaultmessage.devicekeys.EngineException
import com.redvaultsystems.redvaultmessage.devicekeys.OlderProtocolOnly
import com.redvaultsystems.redvaultmessage.devicekeys.SendOutcome
import com.redvaultsystems.redvaultmessage.devicekeys.V3Chat
import android.util.Log
import com.redvaultsystems.redvaultmessage.BuildConfig
import android.net.Uri
import com.redvaultsystems.redvaultmessage.data.ChatSettingsStore
import com.redvaultsystems.redvaultmessage.data.ContactKeyPins
import com.redvaultsystems.redvaultmessage.data.FileDownloadStage
import com.redvaultsystems.redvaultmessage.data.FileRepository
import com.redvaultsystems.redvaultmessage.data.FileSendStage
import com.redvaultsystems.redvaultmessage.data.MessagePaging
import com.redvaultsystems.redvaultmessage.data.GroupInviteRepository
import com.redvaultsystems.redvaultmessage.data.GroupRepository
import com.redvaultsystems.redvaultmessage.data.InviteLinkView
import com.redvaultsystems.redvaultmessage.data.InvitePreview
import com.redvaultsystems.redvaultmessage.data.JoinRequestView
import com.redvaultsystems.redvaultmessage.data.api.FriendDto
import com.redvaultsystems.redvaultmessage.data.HiddenMessagesStore
import com.redvaultsystems.redvaultmessage.data.ReplyTarget
import com.redvaultsystems.redvaultmessage.data.model.ChannelPostRef
import com.redvaultsystems.redvaultmessage.data.model.MessageFolding
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.data.model.ReplyRef
import com.redvaultsystems.redvaultmessage.data.model.StoryReplyRef
import com.redvaultsystems.redvaultmessage.data.model.RichMessage
import com.redvaultsystems.redvaultmessage.data.UserFacingException
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import java.io.File
import com.redvaultsystems.redvaultmessage.data.ConversationRepository
import com.redvaultsystems.redvaultmessage.data.VerifiedContactsStore
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ConversationDto
import com.redvaultsystems.redvaultmessage.data.model.Conversation
import com.redvaultsystems.redvaultmessage.data.model.ConversationMeta
import com.redvaultsystems.redvaultmessage.data.model.Disappearing
import com.redvaultsystems.redvaultmessage.data.model.GroupMemberView
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.data.model.MessageSecurity
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.cancel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import com.redvaultsystems.redvaultmessage.data.Outbox
import com.redvaultsystems.redvaultmessage.data.OutboxItem
import com.redvaultsystems.redvaultmessage.data.PickedFile
import com.redvaultsystems.redvaultmessage.data.TransferInterruptedException
import com.redvaultsystems.redvaultmessage.data.model.ReplyView
import com.redvaultsystems.redvaultmessage.tor.TorController
import kotlinx.coroutines.withContext
import kotlinx.coroutines.suspendCancellableCoroutine
import java.util.concurrent.ConcurrentHashMap
import kotlin.coroutines.resume

data class ChatsState(
    /** Most recently active first. */
    val conversations: List<Conversation> = emptyList(),
    /** Message requests: 1:1 chats from people who aren't friends, waiting to be accepted or ignored. */
    val requests: List<Conversation> = emptyList(),
    val loaded: Boolean = false,
    /** A transient problem (e.g. offline) worth showing; cleared by the next successful refresh. */
    val error: String? = null,
    val readReceipts: Boolean = false,
    /** Tor routing: messages and files waiting in the sealed outbox, and how many of them the server refused. */
    val queued: Int = 0,
    val queuedHeld: Int = 0,
    /** Why the last send on device keys was refused (keys changed, no connection...): stays until the next send, unlike [error]. */
    val sendError: String? = null,
    /** The chat [sendError] is about. */
    val sendErrorChat: String? = null,
    /** Device-key chats: which protocol each one uses ("signal", "olm", "mixed"), learnt from its last send (M5). */
    val v3Protocols: Map<String, String> = emptyMap(),
    /** Device-key messages waiting on the server: a contact's keys changed, or a group key hasn't arrived yet. */
    val v3Held: Int = 0,
)

private const val KEY_CHANGE_PAUSED = "Sending is paused because this contact's safety number changed. Verify it first."

/**
 * Everything that exists only while signed in: unsealed conversation keys,
 * decrypted messages, and the refresh loops. Like the web client there's no
 * push transport, so receiving is re-fetching: every 5 s for the chat list
 * and every 3 s for an open thread, only while the app is on screen.
 *
 * [close] cancels all work and overwrites the conversation keys.
 */
class ChatSession(
    val identity: SelfIdentity,
    private val repository: ConversationRepository,
    private val files: FileRepository,
    private val groups: GroupRepository,
    private val invites: GroupInviteRepository,
    private val settings: ChatSettingsStore,
    private val verifiedContacts: VerifiedContactsStore,
    /** Trust on first use: each contact's first key seen here, so a changed one is flagged. */
    private val keyPins: ContactKeyPins,
    private val hiddenMessages: HiddenMessagesStore,
    private val onSessionExpired: () -> Unit,
    /** Tor routing (docs/tor-routing-plan.md): every send goes through this sealed outbox, and leaves only over Tor. */
    private val outbox: Outbox? = null,
    private val tor: TorController? = null,
    /** Per-device keys (protocol 3): null unless this build has them. */
    private val v3: V3Chat? = null,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val metas = ConcurrentHashMap<String, ConversationMeta>()
    // Survives refreshes, which rebuild messages from the server and would
    // otherwise drop the verification computed for messages sent this session.
    private val securityById = ConcurrentHashMap<String, MessageSecurity>()
    // Communities: who's who among the people seen posting (a community can have a thousand members).
    private class Poster(val view: GroupMemberView?)
    private val communityPosters = ConcurrentHashMap<String, Poster>()

    private val _state = MutableStateFlow(ChatsState(readReceipts = settings.readReceipts))
    val state: StateFlow<ChatsState> = _state.asStateFlow()

    // How many of each open chat's newest messages its refreshes fetch; grows with loadOlder (the desktop's threadWindow_).
    private val windows = ConcurrentHashMap<String, Int>()

    // Unsent message-box text per chat, like the desktop's: memory only, gone when the session ends.
    private val _drafts = MutableStateFlow<Map<String, String>>(emptyMap())
    /** Chat id -> what's typed there and not sent yet, for the chat list's "Draft:". */
    val drafts: StateFlow<Map<String, String>> = _drafts.asStateFlow()

    /** Pictures shown in chats: decrypted on this phone and kept in memory only. */
    val pictures = ChatPictures(download = ::downloadFile, writeDecrypted = files::writeDecrypted)

    // The outbox, oldest first, as last read; and whether a drain is running (one at a time, in order).
    @Volatile private var queuedItems: List<OutboxItem> = emptyList()
    private var draining = false

    init {
        if (outbox != null) {
            scope.launch {
                publishQueue()
                drain()
            }
            // Sends as soon as Tor connects, and every 15 s while it's up (a failed attempt waits for the next round).
            tor?.let { t -> scope.launch { t.becameConnected.collect { drain() } } }
            scope.launch {
                while (true) {
                    delay(DRAIN_INTERVAL_MS)
                    drain()
                }
            }
        }
    }

    /** Refreshes the chat list until cancelled. Run it while the list or a thread is visible. */
    suspend fun pollConversations() {
        while (true) {
            guarded { refreshAll() }
            delay(CONVERSATIONS_POLL_MS)
        }
    }

    /** Refreshes one open thread and marks what's new as seen, until cancelled. */
    suspend fun pollThread(conversationId: String) {
        while (true) {
            guarded { refreshThread(conversationId) }
            delay(THREAD_POLL_MS)
        }
    }

    /** One immediate refresh, e.g. right after accepting a friend request created a chat. */
    suspend fun refreshNow() {
        guarded { refreshAll() }
    }

    fun conversation(id: String): Conversation? = _state.value.conversations.firstOrNull { it.id == id }

    /**
     * "Load older messages": the chat's window grows by a page, and a refresh (now) fetches it.
     * [onDone] runs once it's in, or right away when there's nothing older.
     */
    fun loadOlder(conversationId: String, onDone: () -> Unit = {}) {
        if (conversation(conversationId)?.hasOlder != true) return onDone()
        windows[conversationId] = windowOf(conversationId) + MessagePaging.PAGE_SIZE
        scope.launch {
            guarded { refreshThread(conversationId) }
            onDone()
        }
    }

    /** Chats no longer open anywhere go back to their newest page (as the desktop does on leaving a chat). */
    fun keepWindows(openConversationIds: Set<String>) {
        windows.keys.retainAll(openConversationIds)
    }

    private fun windowOf(conversationId: String): Int = windows[conversationId] ?: MessagePaging.PAGE_SIZE

    fun draftFor(conversationId: String): String = _drafts.value[conversationId].orEmpty()

    /** Blank text clears the chat's draft. */
    fun setDraft(conversationId: String, text: String) {
        _drafts.update { if (text.isBlank()) it - conversationId else it + (conversationId to text) }
    }

    fun send(conversationId: String, text: String, replyTo: Message? = null, onResult: (sent: Boolean) -> Unit) {
        val meta = metas[conversationId] ?: return onResult(false)
        sendRefusal(conversationId)?.let { why ->
            _state.update { it.copy(sendError = why, sendErrorChat = conversationId) }
            return onResult(false)
        }
        val target = replyTo?.let {
            ReplyTarget(
                ReplyRef(
                    messageId = it.id,
                    senderId = if (it.fromMe) identity.userId else it.senderId.ifEmpty { meta.otherUserId },
                    preview = if (it.kind == MessageKind.File) "📎 ${it.text}" else it.text,
                ),
                fromMe = it.fromMe,
            )
        }
        // Replies don't exist on device keys yet.
        if (target != null && refusedOnDeviceKeys(conversationId, OlderProtocolOnly.Reply)) return onResult(false)
        if (outbox != null) {
            // Sealed in the outbox first; it goes out over Tor (now, or once Tor is back) and stays until stored. The drain
            // sends it on device keys when the chat is on them, the usual way otherwise (docs/tor-by-default.md rule 5).
            val clean = text.trimStart('\u0000').trim()
            if (clean.isEmpty()) return onResult(false)
            scope.launch {
                val ok = runCatching { withContext(Dispatchers.IO) { outbox.addText(conversationId, clean, target) } }.isSuccess
                publishQueue()
                onResult(ok)
                drain()
            }
            return
        }
        scope.launch {
            if (_state.value.sendError != null) _state.update { it.copy(sendError = null, sendErrorChat = null) }
            // Device keys first, when this chat is on them: it never also goes out the usual way.
            if (v3 != null && meta.community == null && target == null) {
                val sentV3 = try {
                    sendV3(conversationId, meta, text, v3)
                } catch (e: CancellationException) {
                    throw e
                } catch (e: EngineException) {
                    reportV3(conversationId, e.code, e.message)
                    return@launch onResult(false)
                } catch (e: ApiException) {
                    reportV3(conversationId, e.code, e.message)
                    return@launch onResult(false)
                }
                if (sentV3) return@launch onResult(true)
            }
            val ok = guarded {
                val message = repository.send(meta, text, target)
                message.security?.let { securityById[message.id] = it }
                updateConversation(conversationId) { conversation ->
                    if (conversation.messages.any { it.id == message.id }) conversation
                    else conversation.copy(messages = conversation.messages + message)
                }
            }
            onResult(ok)
        }
    }

    /**
     * A chat on device keys never goes back to protocol 1: [what] is refused, shown in the chat, and true comes back.
     * False: it may go out the usual way. The thread screen hides these actions in such a chat.
     */
    private fun refusedOnDeviceKeys(conversationId: String, what: OlderProtocolOnly): Boolean {
        val refusal = v3?.olderProtocolRefusal(conversationId, what) ?: return false
        reportV3(conversationId, "v3_unavailable", refusal)
        return true
    }

    /** New content whose form or sheet shows the problem itself: a refused key, the key pause, or a chat on device keys. Null: it may go out. */
    private fun contentRefusal(conversationId: String, what: OlderProtocolOnly): String? {
        sendRefusal(conversationId)?.let { return it }
        return v3?.olderProtocolRefusal(conversationId, what)?.let { "Not sent: $it" }
    }

    /**
     * Whether this chat is on device keys (it has sent or received on them here): the thread screen hides what
     * device keys can't carry yet (replies, forwarding, polls, pins, the timer, selecting). Step 2 brings them back.
     */
    fun onDeviceKeys(conversationId: String): Boolean = v3?.pinned(conversationId) == true

    /** Retry of a device-key send that failed: encrypted again (never the same ciphertext) and sent with the same message id. */
    private fun retryV3(record: com.redvaultsystems.redvaultmessage.devicekeys.ChatRecord) {
        val meta = metas[record.conversationId] ?: return
        val chat = v3 ?: return
        scope.launch {
            try {
                val file = record.file
                if (file != null) chat.sendFile(record.conversationId, meta.isGroup, meta.otherUserId, file, record.id)
                else chat.send(record.conversationId, meta.isGroup, meta.otherUserId, record.text, record.id)
                showV3(record.conversationId)
            } catch (e: CancellationException) {
                throw e
            } catch (e: EngineException) {
                reportV3(record.conversationId, e.code, e.message)
            } catch (e: ApiException) {
                reportV3(record.conversationId, e.code, e.message)
            }
        }
    }

    /** Shows why something on device keys didn't go out. [typed]: the person's text is still in the box. */
    private fun reportV3(conversationId: String, code: String, message: String?, typed: Boolean = true): String {
        val problem = v3SendProblem(code, message, typed)
        _state.update { it.copy(sendError = problem, sendErrorChat = conversationId) }
        return problem
    }

    /** What to tell the person when a send on device keys is refused (nothing was sent). */
    private fun v3SendProblem(code: String, message: String?, typed: Boolean): String = when (code) {
        "account_key_changed" -> "Not sent: this contact's device keys changed. Compare the new safety number (Verify) before trusting them."
        "rollback", "equivocation", "uncertified_account_key", "bad_device_list" ->
            "Not sent: this contact's device list doesn't check out. Nothing was sent."
        "contact_has_no_devices" -> "Not sent: this chat uses device keys, but the contact has none listed."
        "member_has_no_devices" -> "Not sent: this group uses device keys, but a member has none listed."
        "signal_keys_missing" -> "Not sent: one of their devices uses Signal sessions, but the server didn't hand out its keys. Try again later."
        "identity_changed", "certificate_mismatch", "untrusted_identity" ->
            "Not sent: one of this contact's devices has new Signal keys. Compare the safety number (Verify) before trusting them."
        "network_error" -> "Not sent: no connection." + if (typed) " Your text is still in the box." else ""
        "v3_unavailable", "phone_not_linked" -> "Not sent: " + (message ?: "device keys aren't available.")
        else -> "Not sent: " + (message ?: "something went wrong") + "."
    }

    /** True when [text] went out on device keys; false means "send it the usual way". A pinned chat that can't throws. */
    private suspend fun sendV3(conversationId: String, meta: ConversationMeta, text: String, v3: V3Chat): Boolean {
        if (timerKeepsOffV3(conversationId, v3)) return false
        val outcome = v3.send(conversationId, meta.isGroup, meta.otherUserId, text)
        if (outcome == null) {
            refuseIfV3Expected(conversationId, v3)
            return false
        }
        if (outcome !is SendOutcome.Sent) return false
        showV3(conversationId)
        if (!meta.isGroup) {
            v3.protocol(conversationId, meta.otherUserId)?.let { p -> _state.update { it.copy(v3Protocols = it.v3Protocols + (conversationId to p)) } }
        }
        return true
    }

    /** A disappearing timer is on: the usual way, or a refusal in a chat that has used device keys. */
    private fun timerKeepsOffV3(conversationId: String, v3: V3Chat): Boolean {
        val timer = conversation(conversationId)?.disappearing ?: Disappearing.Off
        if (timer == Disappearing.Off) return false
        if (v3.pinned(conversationId)) throw EngineException("no_disappearing", "Device keys don't support disappearing messages yet. Turn the timer off to send.")
        return true
    }

    /** Device keys aren't available right now. A chat that has used them never falls back silently. */
    private suspend fun refuseIfV3Expected(conversationId: String, v3: V3Chat) {
        val pinned = v3.pinned(conversationId)
        val unlinked = if (!pinned) v3.accountUsesKeysWithoutThisPhone() else null
        val note = "sendV3 without a device: pinned=$pinned accountUsesKeys=$unlinked"
        if (BuildConfig.DEBUG) Log.w(TAG, note)
        if (pinned) throw EngineException("v3_unavailable", "Device keys aren't available on this phone right now, so this chat can't send. Nothing was sent.")
        // This phone lost (or never had) its device keys while the account uses them: don't quietly send the usual way.
        if (unlinked == true) {
            throw EngineException("phone_not_linked", "This account uses device keys, but this phone isn't linked to them. Open Settings, Devices & encryption to link it.")
        }
    }

    /**
     * Sends a file on device keys when this chat is on them: decided before the upload, then encrypted and uploaded as
     * usual, released, and its descriptor sent in the envelope. False means "send it the usual way". A refusal is shown
     * like a text's and also comes back as a [UserFacingException] for the file row.
     */
    private suspend fun sendFileV3(conversationId: String, meta: ConversationMeta, onStage: (FileSendStage) -> Unit, pick: suspend () -> PickedFile): Boolean {
        val chat = v3 ?: return false
        if (meta.community != null) return false
        try {
            if (_state.value.sendError != null) _state.update { it.copy(sendError = null, sendErrorChat = null) }
            if (timerKeepsOffV3(conversationId, chat)) return false
            val onKeys = chat.usesDeviceKeys(conversationId, meta.isGroup, meta.otherUserId)
            if (onKeys == null) refuseIfV3Expected(conversationId, chat)
            if (onKeys != true) return false
            val descriptor = files.uploadForDeviceKeys(meta.id, pick(), onStage)
            val outcome = chat.sendUploaded(conversationId, meta.isGroup, meta.otherUserId, descriptor, deleteUpload = ::deleteUploadBestEffort)
            if (outcome !is SendOutcome.Sent) throw EngineException("v3_unavailable", "device keys stopped being available for this chat")
            return true
        } catch (e: EngineException) {
            throw UserFacingException(reportV3(conversationId, e.code, e.message, typed = false))
        } catch (e: ApiException) {
            if (e.status == 401) throw e
            throw UserFacingException(reportV3(conversationId, e.code, e.message, typed = false))
        } finally {
            showV3(conversationId)
        }
    }

    /** Removes an upload no message will ever refer to. It stays on the server until it expires if this fails. */
    private suspend fun deleteUploadBestEffort(objectId: String) {
        try {
            files.deleteFile(objectId)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (BuildConfig.DEBUG) Log.w(TAG, "Couldn't delete a device-key file: ${e.javaClass.name}")
        }
    }

    /** Sends a reaction, edit or delete of a device-key message, then [after] once it's out. */
    private fun sendV3Op(conversationId: String, op: ChatOp, typed: Boolean = false, onResult: (sent: Boolean) -> Unit = {}, after: suspend () -> Unit = {}) {
        val meta = metas[conversationId] ?: return onResult(false)
        val chat = v3 ?: return onResult(false)
        scope.launch {
            if (_state.value.sendError != null) _state.update { it.copy(sendError = null, sendErrorChat = null) }
            val sent = try {
                chat.sendOp(conversationId, meta.isGroup, meta.otherUserId, op)
                true
            } catch (e: CancellationException) {
                throw e
            } catch (e: EngineException) {
                reportV3(conversationId, e.code, e.message, typed)
                false
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired() else reportV3(conversationId, e.code, e.message, typed)
                false
            }
            if (sent) after()
            showV3(conversationId)
            onResult(sent)
        }
    }

    /** Brings an open list up to date with this phone's device-key history. */
    private fun showV3(conversationId: String) {
        updateConversation(conversationId) { it.copy(messages = withV3(conversationId, it.messages)) }
    }

    /** [messages] with this chat's device-key messages as they stand now (kept on this phone), oldest first. */
    private fun withV3(conversationId: String, messages: List<Message>): List<Message> {
        val shown = v3?.shown(conversationId).orEmpty()
        if (shown.isEmpty()) return messages
        // Rebuilt each time: a reaction, edit or delete changes a message already in the list.
        val others = messages.filterNot { it.v3 }
        val known = others.mapTo(HashSet()) { it.id.lowercase() }
        val extra = shown.filter { it.record.id !in known }.map { V3Messages.toMessage(it, identity.userId) }
        return (others + extra).sortedBy { it.createdAtMs }
    }

    /**
     * The server's live channel says device-key messages are waiting ({"type":"v3-mailbox"}, no sender, no chat):
     * fetch them now instead of at the next poll. Does nothing in a build without device keys.
     */
    fun listenForV3Wake(events: kotlinx.coroutines.flow.SharedFlow<kotlinx.serialization.json.JsonObject>) {
        if (v3 == null) return
        scope.launch {
            events.collect { event ->
                if ((event["type"] as? kotlinx.serialization.json.JsonPrimitive)?.content == "v3-mailbox") refreshNow()
            }
        }
    }

    /** Pulls this phone's device-key mailbox; a failure never blocks the usual chat refresh. */
    private suspend fun receiveV3() {
        try {
            val report = v3?.receive()
            if (report != null) _state.update { it.copy(v3Held = report.held.size) }
            val note = if (report != null && (report.dropped.isNotEmpty() || report.held.isNotEmpty())) "Device-key mailbox: dropped=${report.dropped} held=${report.held.size}" else null
            if (BuildConfig.DEBUG) note?.let { Log.w(TAG, it) }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (BuildConfig.DEBUG) Log.w(TAG, "Device-key receive failed: ${e.javaClass.name} ${(e as? EngineException)?.code.orEmpty()}")
        }
    }

    /** Sends a reply or a reaction to a story to the friend who posted it (a plain chat message that names the story). */
    fun sendStoryReply(authorId: String, text: String, ref: StoryReplyRef, onResult: (sent: Boolean) -> Unit) {
        val meta = metas.values.firstOrNull { !it.isGroup && it.otherUserId == authorId } ?: return onResult(false)
        sendRefusal(meta.id)?.let { why ->
            _state.update { it.copy(sendError = why, sendErrorChat = meta.id) }
            return onResult(false)
        }
        if (refusedOnDeviceKeys(meta.id, OlderProtocolOnly.StoryReply)) return onResult(false)
        scope.launch {
            val ok = guarded {
                val message = repository.sendStoryReply(meta, text, ref)
                updateConversation(meta.id) { conversation ->
                    if (conversation.messages.any { it.id == message.id }) conversation
                    else conversation.copy(messages = conversation.messages + message)
                }
            }
            onResult(ok)
        }
    }

    /**
     * Encrypts, uploads and sends a file. [onResult] gets null on success or a
     * message that's safe to show. The upload keeps going if the user leaves
     * the chat, and the message appears once it's stored.
     */
    fun sendFile(conversationId: String, uri: Uri, onStage: (FileSendStage) -> Unit, onResult: (error: String?) -> Unit) {
        val meta = metas[conversationId] ?: return onResult("This chat isn't loaded yet. Try again.")
        sendRefusal(conversationId)?.let { return onResult(it) }
        if (outbox != null) return queueFile(conversationId, "file", onStage, onResult) { files.picked(uri) }
        scope.launch {
            try {
                if (sendFileV3(conversationId, meta, onStage) { files.picked(uri) }) return@launch onResult(null)
                val message = files.send(meta, uri, onStage)
                message.security?.let { securityById[message.id] = it }
                updateConversation(conversationId) { conversation ->
                    if (conversation.messages.any { it.id == message.id }) conversation
                    else conversation.copy(messages = conversation.messages + message)
                }
                onResult(null)
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired()
                onResult("Upload failed. Try again.")
            } catch (e: Exception) {
                onResult((e as? UserFacingException)?.message ?: "Upload failed. Try again.")
            }
        }
    }

    /**
     * [sendFile] for a file waiting in the message box: picked or shared ([PendingFile.uri]), or a picture
     * pasted into it and held in memory ([PendingFile.bytes]).
     */
    fun sendPending(conversationId: String, pending: PendingFile, onStage: (FileSendStage) -> Unit, onResult: (error: String?) -> Unit) {
        val bytes = pending.bytes
        val uri = pending.uri
        if (bytes == null) return if (uri != null) sendFile(conversationId, uri, onStage, onResult) else onResult("Couldn't read that file.")
        val picked = PickedFile(pending.name, pending.mimeType ?: "application/octet-stream", bytes.size.toLong()) { bytes.inputStream() }
        val meta = metas[conversationId] ?: return onResult("This chat isn't loaded yet. Try again.")
        sendRefusal(conversationId)?.let { return onResult(it) }
        if (outbox != null) return queueFile(conversationId, "file", onStage, onResult) { picked }
        scope.launch {
            try {
                if (sendFileV3(conversationId, meta, onStage) { picked }) return@launch onResult(null)
                showSent(conversationId, files.sendPicked(meta, picked, onStage))
                onResult(null)
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired()
                onResult("Upload failed. Try again.")
            } catch (e: Exception) {
                onResult((e as? UserFacingException)?.message ?: "Upload failed. Try again.")
            }
        }
    }

    /**
     * Several files from the message box, one after another in order (the desktop's SendFilesDialog queue).
     * It goes on if the user leaves the chat. [onResult] gets how many went out, and the first failure (the
     * rest wait for Retry), or null once all did.
     */
    fun sendPendingBatch(
        conversationId: String,
        batch: List<PendingFile>,
        onStage: (index: Int, FileSendStage) -> Unit,
        onResult: (sent: Int, error: String?) -> Unit,
    ) {
        scope.launch {
            var sent = 0
            for ((index, pending) in batch.withIndex()) {
                val error = suspendCancellableCoroutine { done ->
                    sendPending(conversationId, pending, { stage -> onStage(index, stage) }) { if (done.isActive) done.resume(it) }
                }
                if (error != null) return@launch onResult(sent, error)
                sent++
            }
            onResult(sent, null)
        }
    }

    /** A message this session just stored: into its chat at once, without waiting for the next refresh. */
    private fun showSent(conversationId: String, message: Message) {
        message.security?.let { securityById[message.id] = it }
        updateConversation(conversationId) { conversation ->
            if (conversation.messages.any { it.id == message.id }) conversation
            else conversation.copy(messages = conversation.messages + message)
        }
    }

    /** Sends a recorded voice message, then deletes the recording. Same reporting as [sendFile]. */
    fun sendVoice(conversationId: String, recording: File, onStage: (FileSendStage) -> Unit, onResult: (error: String?) -> Unit) {
        val meta = metas[conversationId] ?: return onResult("This chat isn't loaded yet. Try again.").also { recording.delete() }
        sendRefusal(conversationId)?.let {
            recording.delete()
            return onResult(it)
        }
        if (outbox != null) {
            return queueFile(conversationId, "voice", onStage, { error -> recording.delete(); onResult(error) }) {
                PickedFile(FileRepository.VOICE_FILENAME, FileRepository.VOICE_MIME_TYPE, recording.length()) { recording.inputStream() }
            }
        }
        scope.launch {
            try {
                val voice = PickedFile(FileRepository.VOICE_FILENAME, FileRepository.VOICE_MIME_TYPE, recording.length()) { recording.inputStream() }
                if (sendFileV3(conversationId, meta, onStage) { voice }) return@launch onResult(null)
                val message = files.sendVoice(meta, recording, onStage)
                message.security?.let { securityById[message.id] = it }
                updateConversation(conversationId) { conversation ->
                    if (conversation.messages.any { it.id == message.id }) conversation
                    else conversation.copy(messages = conversation.messages + message)
                }
                onResult(null)
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired()
                onResult("Couldn't send the voice message. Try again.")
            } catch (e: Exception) {
                onResult((e as? UserFacingException)?.message ?: "Couldn't send the voice message. Try again.")
            } finally {
                recording.delete()
            }
        }
    }

    /** Downloads and decrypts a received file into the app's private cache. Throws a [UserFacingException] on failure. */
    suspend fun downloadFile(descriptor: FileDescriptor, onStage: (FileDownloadStage) -> Unit): File {
        try {
            return files.download(descriptor, onStage)
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
            throw UserFacingException("Download failed. Try again.")
        }
    }

    /** Adds the emoji, or removes it if it's already this user's reaction, then refreshes the thread. */
    fun toggleReaction(conversationId: String, message: Message, emoji: String) {
        if (message.v3) {
            val mine = message.reactions.any { it.emoji == emoji && it.mine }
            return sendV3Op(conversationId, ChatOp.reaction(message.id, if (mine) "" else emoji))
        }
        val meta = metas[conversationId] ?: return
        if (refusedOnDeviceKeys(conversationId, OlderProtocolOnly.OlderMessageChange)) return
        val mine = message.reactions.firstOrNull { it.emoji == emoji && it.mine }?.myReactionId
        scope.launch {
            guarded {
                if (mine != null) repository.delete(mine) else repository.sendReaction(meta, message.id, emoji)
                refreshThread(conversationId)
            }
        }
    }

    /** Sends a new text for one of this user's own messages, then refreshes so it shows as edited. */
    fun edit(conversationId: String, messageId: String, text: String, onResult: (sent: Boolean) -> Unit) {
        if (conversation(conversationId)?.messages?.firstOrNull { it.id == messageId }?.v3 == true) {
            val clean = text.trim()
            if (clean.isEmpty()) return onResult(false)
            return sendV3Op(conversationId, ChatOp.edit(messageId, clean), typed = true, onResult = onResult)
        }
        val meta = metas[conversationId] ?: return onResult(false)
        if (refusedOnDeviceKeys(conversationId, OlderProtocolOnly.OlderMessageChange)) return onResult(false)
        scope.launch {
            val ok = guarded {
                repository.sendEdit(meta, messageId, text)
                refreshThread(conversationId)
            }
            onResult(ok)
        }
    }

    /**
     * Forwards messages to other chats, oldest first, like the desktop's forwardMessages: text as forwarded
     * text, a file as a new copy with a new key for each chat (decrypted once; sending files is Pro-only,
     * and the server enforces it). [onResult] gets null once everything is sent, or a message that's safe
     * to show (what went out before the failure stays sent).
     */
    fun forwardMany(messages: List<Message>, toConversationIds: List<String>, onResult: (error: String?) -> Unit) {
        val targets = toConversationIds.distinct().map { metas[it] ?: return onResult("That chat isn't loaded yet. Try again.") }
        val items = messages.filter(MessageSelection::canForward).sortedBy { it.createdAtMs }
        if (items.isEmpty() || targets.isEmpty()) return onResult(null)
        // Before anything goes out, so a chat that can't take it doesn't leave the rest half done.
        targets.firstNotNullOfOrNull { contentRefusal(it.id, OlderProtocolOnly.Forward) }?.let { return onResult(it) }
        scope.launch {
            try {
                for (message in items) {
                    val descriptor = message.file
                    if (descriptor != null) {
                        files.forwardToMany(targets, descriptor) { meta, sent -> showSent(meta.id, sent) }
                        continue
                    }
                    // A post forwarded from a channel stays one, so every copy keeps counting toward its views.
                    val channelPost = message.channelPost
                    for (meta in targets) {
                        val sent = if (channelPost != null) repository.sendChannelForward(meta, message.text, channelPost)
                        else repository.sendForwarded(meta, message.text)
                        showSent(meta.id, sent)
                    }
                }
                onResult(null)
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired()
                onResult("Couldn't forward it. Try again.")
            } catch (e: Exception) {
                onResult((e as? UserFacingException)?.message ?: "Couldn't forward it. Try again.")
            }
        }
    }

    /**
     * Sends a channel post into a chat, end-to-end encrypted like any message,
     * as a post from that channel. [onResult] gets null on success or a
     * message that's safe to show.
     */
    fun forwardChannelPost(toConversationId: String, text: String, ref: ChannelPostRef, onResult: (error: String?) -> Unit) {
        val meta = metas[toConversationId] ?: return onResult("That chat isn't loaded yet. Try again.")
        if (text.trimStart('\u0000').isBlank()) return onResult("There's no text in this post to forward.")
        contentRefusal(toConversationId, OlderProtocolOnly.Forward)?.let { return onResult(it) }
        scope.launch {
            try {
                val sent = repository.sendChannelForward(meta, text, ref)
                sent.security?.let { securityById[sent.id] = it }
                updateConversation(toConversationId) { conversation ->
                    if (conversation.messages.any { it.id == sent.id }) conversation
                    else conversation.copy(messages = conversation.messages + sent)
                }
                onResult(null)
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) onSessionExpired()
                onResult("Couldn't forward it. Try again.")
            } catch (e: Exception) {
                onResult((e as? UserFacingException)?.message ?: "Couldn't forward it. Try again.")
            }
        }
    }

    /** Pins the message for everyone in the chat, or unpins it if it's pinned. */
    fun togglePin(conversationId: String, message: Message) {
        val meta = metas[conversationId] ?: return
        if (refusedOnDeviceKeys(conversationId, OlderProtocolOnly.Pin)) return
        val pinned = MessageFolding.pinnedFrom(conversation(conversationId)?.messages.orEmpty()).any { it.messageId == message.id.lowercase() }
        val preview = when {
            message.kind == MessageKind.File -> "📎 ${message.text}"
            message.poll != null -> "📊 ${message.text}"
            else -> message.text
        }
        scope.launch {
            guarded {
                if (pinned) repository.sendUnpin(meta, message.id) else repository.sendPin(meta, message.id, preview)
                refreshThread(conversationId)
            }
        }
    }

    fun unpin(conversationId: String, messageId: String) {
        val meta = metas[conversationId] ?: return
        if (refusedOnDeviceKeys(conversationId, OlderProtocolOnly.Pin)) return
        scope.launch {
            guarded {
                repository.sendUnpin(meta, messageId)
                refreshThread(conversationId)
            }
        }
    }

    /** [onResult] gets null once it's sent, or a message that's safe to show. */
    fun createPoll(conversationId: String, question: String, options: List<String>, multiple: Boolean, onResult: (error: String?) -> Unit) {
        val meta = metas[conversationId] ?: return onResult("This chat isn't loaded yet. Try again.")
        if (runCatching { RichMessage.serializePoll(question, options, multiple) }.isFailure) {
            return onResult("A poll needs a question and 2 to 10 different options.")
        }
        contentRefusal(conversationId, OlderProtocolOnly.Poll)?.let { return onResult(it) }
        scope.launch {
            val ok = guarded {
                repository.sendPoll(meta, question, options, multiple)
                refreshThread(conversationId)
            }
            onResult(if (ok) null else "Couldn't send the poll. Try again.")
        }
    }

    /** Replaces this user's vote with [options] (empty takes it back). */
    fun vote(conversationId: String, poll: Message, options: List<Int>) {
        val meta = metas[conversationId] ?: return
        val labels = poll.poll?.options ?: return
        if (refusedOnDeviceKeys(conversationId, OlderProtocolOnly.Vote)) return
        scope.launch {
            guarded {
                repository.sendVote(meta, poll.id, options, labels)
                refreshThread(conversationId)
            }
        }
    }

    /** Only the poll's creator can end it; everyone else ignores an end from anyone else. */
    fun endPoll(conversationId: String, pollId: String) {
        val meta = metas[conversationId] ?: return
        if (refusedOnDeviceKeys(conversationId, OlderProtocolOnly.Vote)) return
        scope.launch {
            guarded {
                repository.sendEndPoll(meta, pollId)
                refreshThread(conversationId)
            }
        }
    }

    /** "Delete for me": hidden on this phone only; the other person keeps it. */
    fun deleteForMe(conversationId: String, messageId: String) {
        hiddenMessages.hide(identity.userId, messageId)
        updateConversation(conversationId) { c -> c.copy(messages = c.messages.filterNot { it.id == messageId }) }
    }

    fun delete(conversationId: String, messageId: String) {
        // Its edits and your own pin notices for it go too: both repeat its
        // text, and apps that don't fold them would keep showing it.
        val messages = conversation(conversationId)?.messages.orEmpty()
        val target = messages.firstOrNull { it.id == messageId }
        if (target?.v3 == true) {
            val objectId = target.file?.objectId?.takeIf { target.fromMe }
            return sendV3Op(conversationId, ChatOp.delete(messageId)) {
                // Best effort: everyone has dropped the key by now, whatever happens to the ciphertext.
                if (objectId != null) {
                    try {
                        files.deleteFile(objectId)
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        if (BuildConfig.DEBUG) Log.w(TAG, "Couldn't delete a device-key file: ${e.javaClass.name}")
                    }
                }
            }
        }
        val related = target?.editMessageIds.orEmpty() +
            messages.filter { it.fromMe && it.notice?.targetId == messageId.lowercase() }.map { it.id }
        scope.launch {
            guarded {
                repository.delete(messageId)
                related.forEach { repository.delete(it) }
                val gone = related.toSet() + messageId
                updateConversation(conversationId) { c -> c.copy(messages = c.messages.filterNot { it.id in gone }) }
            }
        }
    }

    /** Several ticked messages, "Delete for me": hidden on this phone only. */
    fun deleteManyForMe(conversationId: String, messageIds: Set<String>) {
        messageIds.forEach { hiddenMessages.hide(identity.userId, it) }
        updateConversation(conversationId) { c -> c.copy(messages = c.messages.filterNot { it.id in messageIds }) }
    }

    /** Several of your own ticked messages, deleted for everyone with their edits and pin notices (others' are skipped). */
    fun deleteManyForEveryone(conversationId: String, messageIds: Set<String>) {
        val ids = MessageSelection.deletions(conversation(conversationId)?.messages.orEmpty(), messageIds)
        if (ids.isEmpty()) return
        scope.launch {
            val gone = HashSet<String>()
            guarded {
                for (id in ids) {
                    repository.delete(id)
                    gone += id
                }
            }
            // What went before a failure is gone for everyone already.
            updateConversation(conversationId) { c -> c.copy(messages = c.messages.filterNot { it.id in gone }) }
            guarded { refreshThread(conversationId) }
        }
    }

    /** Optimistic: shows the new timer at once and reverts if the server refuses. */
    fun setDisappearing(conversationId: String, duration: Disappearing) {
        val previous = conversation(conversationId)?.disappearing ?: return
        updateConversation(conversationId) { it.copy(disappearing = duration) }
        scope.launch {
            val ok = guarded { repository.setDisappearing(conversationId, duration) }
            if (!ok) updateConversation(conversationId) { it.copy(disappearing = previous) }
        }
    }

    /** Computes stage-3 verification for an own message that doesn't have it yet (loaded from a refresh). */
    fun requestVerification(conversationId: String, messageId: String) {
        val message = conversation(conversationId)?.messages?.firstOrNull { it.id == messageId } ?: return
        if (message.security != null || !message.fromMe) return
        scope.launch {
            val security = repository.verify(message)
            securityById[messageId] = security
            updateConversation(conversationId) { c ->
                c.copy(messages = c.messages.map { if (it.id == messageId) it.copy(security = security) else it })
            }
        }
    }

    fun setContactVerified(conversationId: String, verified: Boolean) {
        val meta = metas[conversationId]?.takeIf { !it.isGroup } ?: return
        verifiedContacts.setVerified(identity.userId, meta.otherPublicKeyB64, verified)
        // Verifying the new key is what clears a key-change warning.
        if (verified) keyPins.accept(identity.userId, meta.otherUserId, meta.otherPublicKeyB64)
        updateConversation(conversationId) { it.copy(verified = verified, keyChanged = keyChanged(meta, verified)) }
    }

    /** Why nothing can go out in this chat right now, or null: its key was refused, or a contact's key changed. */
    private fun sendRefusal(conversationId: String): String? =
        repository.keyProblem(conversationId) ?: KEY_CHANGE_PAUSED.takeIf { keyChangePaused(conversationId) }

    /** Account keys never change in normal use, so a new one holds sending until it's verified. */
    private fun keyChangePaused(conversationId: String): Boolean =
        _state.value.conversations.firstOrNull { it.id == conversationId }?.keyChanged == true

    private fun keyChanged(meta: ConversationMeta, verified: Boolean): Boolean =
        !meta.isGroup && keyPins.keyChanged(identity.userId, meta.otherUserId, meta.otherPublicKeyB64, verified)

    // --- Groups. Each throws on failure with a message that's safe to show. ---

    suspend fun friends(): List<FriendDto> = groupCall { groups.friends() }

    /** Creates a group and returns its id, once it's in the chat list. */
    suspend fun createGroup(name: String, members: List<FriendDto>): String = groupCall {
        groups.create(identity, name, members).also { refreshAll() }
    }

    suspend fun addToGroup(conversationId: String, friend: FriendDto) = groupCall {
        groups.add(metas[conversationId] ?: throw UserFacingException("This group isn't loaded yet."), friend)
        reloadMeta(conversationId)
    }

    suspend fun removeFromGroup(conversationId: String, userId: String) = groupCall {
        groups.remove(conversationId, userId, identity.userId)
        reloadMeta(conversationId)
    }

    suspend fun leaveGroup(conversationId: String) = groupCall {
        groups.remove(conversationId, identity.userId, identity.userId)
        metas.remove(conversationId)
        _state.update { s -> s.copy(conversations = s.conversations.filterNot { it.id == conversationId }) }
    }

    suspend fun renameGroup(conversationId: String, name: String) = groupCall {
        groups.rename(metas[conversationId] ?: throw UserFacingException("This group isn't loaded yet."), name)
        reloadMeta(conversationId)
    }

    suspend fun setGroupRole(conversationId: String, userId: String, role: String) = groupCall {
        groups.setRole(conversationId, userId, role)
        reloadMeta(conversationId)
    }

    // --- Group invite links (data/GroupInviteRepository.kt). Each throws with a message that's safe to show. ---

    /** Any member: the group's link, or null when it has none. */
    suspend fun inviteLink(conversationId: String): InviteLinkView? = groupCall { invites.link(groupMeta(conversationId)) }

    /** Owner/admin: turns the link on, or resets it, keeping [approval]. */
    suspend fun newInviteLink(conversationId: String, approval: Boolean): InviteLinkView = groupCall {
        invites.newLink(groupMeta(conversationId), approval)
    }

    suspend fun setInviteApproval(conversationId: String, approval: Boolean, secret: ByteArray): InviteLinkView = groupCall {
        invites.setApproval(groupMeta(conversationId), approval, secret)
    }

    suspend fun turnOffInviteLink(conversationId: String) = groupCall { invites.turnOff(conversationId) }

    suspend fun joinRequests(conversationId: String): List<JoinRequestView> = groupCall { invites.requests(conversationId) }

    suspend fun approveJoinRequest(conversationId: String, userId: String) = groupCall {
        invites.approve(groupMeta(conversationId), userId)
        reloadMeta(conversationId)
    }

    suspend fun declineJoinRequest(conversationId: String, userId: String) = groupCall { invites.deny(conversationId, userId) }

    /** Someone else's link: the group's name and size (see [GroupInviteRepository.open]). */
    suspend fun openInvite(linkId: String, secret: String): InvitePreview = groupCall { invites.open(linkId, secret) }

    /** Joins, or asks to join; once in, the group is in the chat list. Returns "joined", "member" or "requested". */
    suspend fun joinFromInvite(preview: InvitePreview): String = groupCall {
        val result = invites.join(identity, preview)
        if (result.status != "requested") refreshAll()
        result.status
    }

    /** Takes back your own request to join. */
    suspend fun cancelJoinRequest(preview: InvitePreview) = groupCall { invites.deny(preview.conversationId, identity.userId) }

    private fun groupMeta(conversationId: String): ConversationMeta =
        metas[conversationId]?.takeIf { it.isGroup } ?: throw UserFacingException("This group isn't loaded yet.")

    /** Membership or name changed: re-resolve the group now instead of waiting for the next minute's refresh. */
    private suspend fun reloadMeta(conversationId: String) {
        metas.remove(conversationId)
        refreshAll()
    }

    private suspend fun <T> groupCall(block: suspend () -> T): T {
        try {
            return block()
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
            throw UserFacingException(e.message?.takeIf { it.isNotBlank() } ?: "Something went wrong. Try again.")
        }
    }

    fun setReadReceipts(enabled: Boolean) {
        settings.readReceipts = enabled
        _state.update { it.copy(readReceipts = enabled) }
    }

    /** Retry: a message the server refused goes out again with the next drain. */
    fun retryQueued(id: String) {
        val failed = v3?.failed(id)
        if (failed != null) return retryV3(failed)
        val box = outbox ?: return
        scope.launch {
            withContext(Dispatchers.IO) { box.release(id) }
            publishQueue()
            drain()
        }
    }

    /** Discard: a queued message is deleted from this phone without being sent. */
    fun discardQueued(id: String) {
        val failed = v3?.failed(id)
        if (failed != null) {
            v3?.discard(failed.conversationId, failed.id)
            updateConversation(failed.conversationId) { c -> c.copy(messages = c.messages.filterNot { it.id == failed.id }) }
            // A file nobody will now receive: its upload goes too.
            failed.file?.objectId?.let { objectId -> scope.launch { deleteUploadBestEffort(objectId) } }
            return
        }
        val box = outbox ?: return
        scope.launch {
            withContext(Dispatchers.IO) { box.remove(id) }
            publishQueue()
        }
    }

    /** Sends whatever is queued, now. */
    fun drainNow() {
        scope.launch { drain() }
    }

    private fun queueFile(
        conversationId: String,
        kind: String,
        onStage: (FileSendStage) -> Unit,
        onResult: (error: String?) -> Unit,
        pick: suspend () -> PickedFile,
    ) {
        val box = outbox ?: return
        scope.launch {
            try {
                onStage(FileSendStage.Encrypting)
                val picked = pick()
                val limit = files.maxFileSizeIfKnown
                if (limit != null && picked.size != null && picked.size > limit) {
                    throw UserFacingException("File too large. The limit is ${limit / (1024 * 1024)} MB.")
                }
                withContext(Dispatchers.IO) { box.addFile(conversationId, kind, FileMessage.sanitizeFilename(picked.name), picked.mimeType, picked.open) }
                publishQueue()
                onResult(null)
                drain()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                onResult((e as? UserFacingException)?.message ?: "Couldn't queue the file. Try again.")
            }
        }
    }

    /**
     * Sends the outbox in order, one item at a time, while Tor is up. A network
     * failure (Tor down, no connection) stops the round, and everything waits;
     * a refusal holds that one item (Retry or Discard). An item leaves the
     * outbox only once the server stored it, and its id is the clientMessageId,
     * so a repeat after a lost answer is never stored twice.
     */
    private suspend fun drain() {
        val box = outbox ?: return
        if (draining) return
        if (tor != null && tor.gate() == null) return
        draining = true
        try {
            // What device keys already encrypted goes out first, in order; while any of it can't, everything waits.
            if (v3 != null && !flushDeviceKeyQueue()) return
            val items = withContext(Dispatchers.IO) { box.items() }.filter { !it.held }
            for (item in items) {
                // A chat not loaded yet (first refresh still out) waits for the next round.
                val meta = metas[item.conversationId] ?: continue
                // Device keys first, when this chat is on them: it never also goes out the usual way.
                when (sendQueuedOnDeviceKeys(item, meta, box)) {
                    QueuedV3.Sent, QueuedV3.Held -> continue
                    QueuedV3.Wait -> return
                    QueuedV3.UsualWay -> Unit
                }
                try {
                    val message = if (item.kind == "text") {
                        val reply = item.replyToId?.let { ReplyTarget(ReplyRef(it, item.replyToSender ?: "", item.replyPreview ?: ""), item.replyFromMe) }
                        repository.send(meta, item.text, reply, clientMessageId = item.id)
                    } else {
                        files.sendFromOutbox(meta, item.text, item.mimeType ?: "application/octet-stream", item.id) { plain -> box.openFile(item, plain) }
                    }
                    withContext(Dispatchers.IO) { box.remove(item.id) }
                    message.security?.let { securityById[message.id] = it }
                    updateConversation(item.conversationId) { c ->
                        if (c.messages.any { it.id == message.id }) c else c.copy(messages = c.messages.filterNot { it.id == item.id } + message)
                    }
                    publishQueue()
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    if (e is ApiException && e.status == 401) {
                        onSessionExpired()
                        return
                    }
                    val transient = isTransient(e)
                    val shown = (e as? UserFacingException)?.message ?: (e as? ApiException)?.message ?: "Couldn't send it."
                    withContext(Dispatchers.IO) { box.recordAttempt(item.id, shown, hold = !transient) }
                    publishQueue()
                    if (transient) return // Tor or the server is down: the rest wait too, in order
                }
            }
        } finally {
            draining = false
        }
    }

    private enum class QueuedV3 { Sent, Held, Wait, UsualWay }

    /**
     * An outbox item on device keys, by the step-1 rule (docs/signal-integration/07-step1-default-path.md section 2),
     * over Tor like everything else. Encrypted once: if the post itself can't go out now, it waits, encrypted, in the
     * device-key queue (V3SendQueue) and the outbox item is done. [QueuedV3.UsualWay]: this chat isn't on device keys.
     */
    private suspend fun sendQueuedOnDeviceKeys(item: OutboxItem, meta: ConversationMeta, box: Outbox): QueuedV3 {
        val chat = v3 ?: return QueuedV3.UsualWay
        if (meta.community != null || item.replyToId != null) return QueuedV3.UsualWay
        try {
            if (timerKeepsOffV3(item.conversationId, chat)) return QueuedV3.UsualWay
            val outcome = if (item.kind == "text") {
                chat.send(item.conversationId, meta.isGroup, meta.otherUserId, item.text, item.id)
            } else {
                when (chat.usesDeviceKeys(item.conversationId, meta.isGroup, meta.otherUserId)) {
                    true -> {
                        val descriptor = files.uploadFromOutboxForDeviceKeys(meta.id, item.text, item.mimeType ?: "application/octet-stream") { plain ->
                            box.openFile(item, plain)
                        }
                        chat.sendUploaded(item.conversationId, meta.isGroup, meta.otherUserId, descriptor, item.id, ::deleteUploadBestEffort)
                    }
                    false -> SendOutcome.Protocol1
                    null -> null
                }
            }
            if (outcome == null) {
                refuseIfV3Expected(item.conversationId, chat)
                return QueuedV3.UsualWay
            }
            if (outcome !is SendOutcome.Sent) return QueuedV3.UsualWay
            withContext(Dispatchers.IO) { box.remove(item.id) }
            if (!meta.isGroup) {
                chat.protocol(item.conversationId, meta.otherUserId)?.let { p -> _state.update { it.copy(v3Protocols = it.v3Protocols + (item.conversationId to p)) } }
            }
            showV3(item.conversationId)
            publishQueue()
            return QueuedV3.Sent
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (e is ApiException && e.status == 401) {
                onSessionExpired()
                return QueuedV3.Wait
            }
            // The outbox keeps the item (it shows the problem, with Retry and Discard); a failed device-key copy of it
            // would show twice.
            chat.failed(item.id)?.let { chat.discard(it.conversationId, it.id) }
            val transient = isTransient(e)
            val shown = when (e) {
                is EngineException -> v3SendProblem(e.code, e.message, typed = false)
                is ApiException -> if (transient) e.message ?: "Couldn't send it." else v3SendProblem(e.code, e.message, typed = false)
                else -> (e as? UserFacingException)?.message ?: "Couldn't send it."
            }
            withContext(Dispatchers.IO) { box.recordAttempt(item.id, shown, hold = !transient) }
            publishQueue()
            return if (transient) QueuedV3.Wait else QueuedV3.Held
        }
    }

    /** Sends the device-key queue (already encrypted, in order). True when nothing is left waiting. */
    private suspend fun flushDeviceKeyQueue(): Boolean {
        val chat = v3 ?: return true
        val done = try {
            chat.flushQueue()
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) onSessionExpired()
            false
        } catch (e: Exception) {
            false
        }
        metas.keys.forEach(::showV3)
        publishQueue()
        return done
    }

    /** Re-reads the outbox and shows its items in their chats, after the server's messages. */
    private suspend fun publishQueue() {
        val box = outbox ?: return
        val items = withContext(Dispatchers.IO) { box.items() }
        queuedItems = items
        _state.update { s ->
            s.copy(
                conversations = s.conversations.map { c -> c.copy(messages = withQueued(c.id, c.messages)) },
                queued = items.size + (v3?.queuedMessages() ?: 0),
                queuedHeld = items.count { it.held },
            )
        }
    }

    private fun withQueued(conversationId: String, messages: List<Message>): List<Message> {
        val mine = queuedItems.filter { it.conversationId == conversationId }
        val base = messages.filterNot { it.queued }
        if (mine.isEmpty()) return base
        return base + mine.map { item ->
            Message(
                id = item.id,
                fromMe = true,
                senderId = identity.userId,
                kind = if (item.kind == "text") MessageKind.Text else MessageKind.File,
                text = item.text,
                replyTo = item.replyToId?.let { ReplyView(it, RichMessage.makePreview(item.replyPreview ?: ""), item.replyFromMe) },
                createdAtMs = item.createdAtMs,
                deleteAtMs = null,
                queued = true,
                queueError = item.lastError,
                queueHeld = item.held,
            )
        }
    }

    private fun isTransient(e: Exception): Boolean = when (e) {
        is TransferInterruptedException -> true
        is ApiException -> e.status == 0 || e.status >= 500 || e.status == 429 || e.code == "network_error" || e.code == "tor_unavailable"
        is java.io.IOException -> true
        else -> false
    }

    fun close() {
        scope.cancel()
        v3?.close()
        outbox?.close()
        files.clearCache()
        metas.values.forEach { it.conversationKey.fill(0) }
        metas.clear()
        securityById.clear()
        communityPosters.clear()
        pictures.clear()
        windows.clear()
        _drafts.value = emptyMap()
        _state.value = ChatsState()
    }

    private suspend fun refreshAll() {
        receiveV3()
        val dtos = repository.listConversations()
        // Message requests: chats non-friends started. A failure here (an older server, a blip) never hides the chat list.
        val requestDtos = try {
            repository.listRequests()
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) throw e
            emptyList()
        } catch (e: Exception) {
            emptyList()
        }
        val conversations = loadAll(dtos)
        val requests = loadAll(requestDtos)
        val liveIds = (dtos + requestDtos).mapTo(HashSet()) { it.id }
        metas.keys.retainAll(liveIds)
        _state.update { state ->
            state.copy(
                conversations = conversations.sortedByDescending { it.lastActivityMs },
                requests = requests.sortedByDescending { it.lastActivityMs },
                loaded = true,
                error = null,
            )
        }
    }

    private suspend fun loadAll(dtos: List<ConversationDto>): List<Conversation> = coroutineScope {
        dtos.map { dto ->
            async {
                // One conversation that can't be opened (or fails to load) is
                // left out of this refresh; the others still load.
                try {
                    loadConversation(dto)
                } catch (e: CancellationException) {
                    throw e
                } catch (e: ApiException) {
                    if (e.status == 401) throw e
                    if (BuildConfig.DEBUG) Log.w(TAG, "A conversation didn't load: ${e.code}")
                    null
                } catch (e: Exception) {
                    if (BuildConfig.DEBUG) Log.w(TAG, "A conversation didn't load: ${e.javaClass.name}")
                    null
                }
            }
        }.awaitAll()
    }.filterNotNull()

    /**
     * Accepts a message request (the chat joins the list and [onAccepted] gets its id) or ignores it (hidden
     * without telling them). Reading a request sends nothing back: nobody learns it was seen until you accept.
     */
    fun respondToRequest(conversationId: String, accept: Boolean, onDone: (ok: Boolean) -> Unit = {}) {
        scope.launch {
            val ok = guarded {
                repository.respondToRequest(conversationId, accept)
                _state.update { state -> state.copy(requests = state.requests.filterNot { it.id == conversationId }) }
                refreshAll()
            }
            onDone(ok)
        }
    }

    private suspend fun loadConversation(dto: ConversationDto): Conversation? {
        if (dto.community != null) return loadCommunity(dto)
        val cached = metas[dto.id]?.takeIf { System.currentTimeMillis() - it.fetchedAtMs < META_REFRESH_MS }
        // A group's key can change at any time (someone left): keep its keys current on every load.
        if (cached != null) repository.refreshKeys(dto, identity)
        // A contact's username or key can change; re-resolve them now and then.
        val meta = cached ?: repository.hydrateMeta(dto, identity)?.also { metas[dto.id] = it } ?: return null
        // An open chat's older messages stay loaded across list refreshes too.
        val result = repository.fetchMessages(meta, identity.userId, windowOf(dto.id))
        val verified = !meta.isGroup && verifiedContacts.isVerified(identity.userId, meta.otherPublicKeyB64)
        return Conversation(
            meta = meta,
            disappearing = Disappearing.fromSeconds(dto.disappearingSeconds),
            messages = withV3(dto.id, withQueued(dto.id, result.messages.withKnownSecurity())),
            unread = result.unread + (v3?.unread(dto.id) ?: 0),
            verified = verified,
            keyChanged = keyChanged(meta, verified),
            hasOlder = result.hasOlder,
            keyProblem = repository.keyProblem(dto.id),
        )
    }

    /**
     * A community: rebuilt on every refresh (no network unless the list reports a new key epoch), so a new
     * epoch, role or posting right shows at once. One that can't be read right now (a membership that just
     * ended, keys not loadable) is left out instead of failing the whole list, as on the web.
     */
    private suspend fun loadCommunity(dto: ConversationDto): Conversation? {
        val (loaded, result) = try {
            val base = repository.hydrateCommunity(dto, identity, metas[dto.id])
            val result = repository.fetchMessages(base, identity.userId, windowOf(dto.id))
            withPosters(base, result.messages) to result
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (e is ApiException && e.status == 401) throw e
            if (BuildConfig.DEBUG) Log.w(TAG, "Couldn't load a community: ${e.javaClass.name} ${(e as? ApiException)?.code.orEmpty()}")
            return null
        }
        metas[dto.id] = loaded
        return Conversation(
            meta = loaded,
            disappearing = Disappearing.fromSeconds(dto.disappearingSeconds),
            messages = result.messages.withKnownSecurity(),
            unread = result.unread,
            verified = false,
            hasOlder = result.hasOlder,
        )
    }

    /** Communities: [meta] plus the people seen posting in [messages], looked up once per session. */
    private suspend fun withPosters(meta: ConversationMeta, messages: List<Message>): ConversationMeta {
        if (meta.community == null) return meta
        val unknown = messages.map { it.senderId }
            .filter { id -> id.isNotEmpty() && id != identity.userId && meta.members.none { it.userId == id } }
            .distinct()
        if (unknown.isEmpty()) return meta
        val found = coroutineScope {
            unknown.map { id ->
                async {
                    communityPosters[id]?.let { return@async it.view }
                    repository.communityPoster(id).also { communityPosters[id] = Poster(it) }
                }
            }.awaitAll().filterNotNull()
        }
        return if (found.isEmpty()) meta else meta.withMembers(found)
    }

    private suspend fun refreshThread(conversationId: String) {
        val meta = metas[conversationId] ?: return
        receiveV3()
        val result = repository.fetchMessages(meta, identity.userId, windowOf(conversationId))
        if (result.unread > 0) repository.markRead(conversationId, _state.value.readReceipts)
        v3?.markRead(conversationId)
        val named = withPosters(meta, result.messages)
        if (named !== meta) metas[conversationId] = named
        updateConversation(conversationId) {
            it.copy(meta = named, messages = withV3(conversationId, withQueued(conversationId, result.messages.withKnownSecurity())), unread = 0, hasOlder = result.hasOlder, keyProblem = repository.keyProblem(conversationId))
        }
    }

    private fun List<Message>.withKnownSecurity(): List<Message> =
        map { message -> securityById[message.id]?.let { message.copy(security = it) } ?: message }

    private fun updateConversation(id: String, transform: (Conversation) -> Conversation) {
        _state.update { state ->
            state.copy(conversations = state.conversations.map { if (it.id == id) transform(it) else it })
        }
    }

    /** Runs [block]; returns false on failure, surfacing it (or ending the session on a 401). */
    private suspend fun guarded(block: suspend () -> Unit): Boolean {
        return try {
            block()
            if (_state.value.error != null) _state.update { it.copy(error = null) }
            true
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) {
                onSessionExpired()
            } else {
                _state.update { it.copy(error = e.message ?: "Something went wrong.") }
            }
            false
        } catch (e: UserFacingException) {
            _state.update { it.copy(error = e.message ?: "Something went wrong.") }
            false
        } catch (e: Exception) {
            // Debug builds only: the type and message are enough to diagnose
            // (e.g. a response shape change) without logging any content.
            if (BuildConfig.DEBUG) Log.w(TAG, "Chat refresh failed: ${e.javaClass.name}: ${e.message?.take(300)}")
            _state.update { it.copy(error = "Something went wrong.") }
            false
        }
    }

    private companion object {
        const val TAG = "ChatSession"
        const val CONVERSATIONS_POLL_MS = 5_000L
        const val THREAD_POLL_MS = 3_000L
        const val META_REFRESH_MS = 60_000L
        const val DRAIN_INTERVAL_MS = 15_000L
    }
}
