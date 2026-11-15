// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.withTimeout
import java.io.File
import java.util.UUID

/**
 * What can only go out on protocol 1 for now, and what a chat on device keys says when it refuses that. Replies,
 * forwards, polls, votes and pins go on device keys since part A (docs/signal-integration/08-part-a-chat-features.md),
 * story replies and reactions since part D (docs/signal-integration/12-part-d-stories.md).
 */
enum class OlderProtocolOnly(val refusal: String) {
    OlderMessageChange("this message was sent before the chat moved to device keys, so it can't be changed or reacted to now. Nothing was sent."),
}

/**
 * The chat screens' door to the v3 chat path (docs/multi-device-threat-model.md section 9): active when this build
 * has the engine, the server has protocol 3 on and this phone is a trusted device; otherwise every call says
 * "not mine". A chat picks device keys by itself (docs/signal-integration/07-step1-default-path.md section 2):
 * every member has a signed device list → device keys; otherwise protocol 1, only while the chat isn't pinned.
 * History is kept per device, encrypted under a key derived from its storage key, never uploaded.
 *
 * [outgoing] (docs/tor-by-default.md rule 5): the device's Tor-routed API with its sealed queue. A message encrypted
 * while Tor is down waits there, already encrypted, and shows as queued until [flushQueue] gets it to the server.
 */
class V3Chat(
    private val keys: DeviceKeysSession,
    private val userId: String,
    private val dir: File,
    private val outgoing: QueueingV3Api? = null,
    /** Part C2: the community routes (recipients, membership tasks, admit), through the same Tor-routed client. */
    private val communityApi: CommunityDeviceApi? = null,
) {
    private val lock = Mutex()

    /** Part D: stories over device keys (keys, named and anonymous answers), under this chat's lock and through its queue. */
    val stories = V3Stories(keys, userId, dir, lock, outgoing)

    // Part C2: the community conversations the chat list loaded (memory only), and the owner and staff task cursors.
    private val communities = java.util.concurrent.ConcurrentHashMap<String, CommunityContext>()
    private val cursors = CommunityCursors(File(dir, "community-cursors-$userId.txt"))

    /** The chat list loaded (or refreshed) a community conversation: its kind and this account's role. */
    fun setCommunity(context: CommunityContext) {
        communities[context.conversationId.lowercase()] = context.copy(conversationId = context.conversationId.lowercase())
    }

    /** Only the communities in [conversationIds] are kept (left, removed or expired ones go). */
    fun keepCommunities(conversationIds: Set<String>) {
        val keep = conversationIds.mapTo(HashSet()) { it.lowercase() }
        communities.keys.retainAll(keep)
    }

    fun community(conversationId: String): CommunityContext? = communities[conversationId.lowercase()]

    private fun routeFor(conversationId: String, isGroup: Boolean): CommunityRoute? =
        if (!isGroup) null else community(conversationId)?.let { CommunityRoute(it, communityApi) }

    /**
     * One round of the owner and staff membership tasks (Part C2): admit, rosters, sender-key rotation. Null when
     * device keys or the community routes aren't available here. Runs under the chat lock, like a send.
     */
    suspend fun runCommunityTasks(): MembershipTaskReport? = lock.withLock {
        val api = communityApi ?: return null
        val (device, _) = ready() ?: return null
        runMembershipTasks(device, api, cursors)
    }
    @Volatile private var history: ChatHistory? = null
    @Volatile private var historyDevice: String? = null

    // Which chats have used device keys, kept beside (not inside) the encrypted history so a chat stays pinned
    // even while the history can't be opened (offline at start, the server's switch off, the key unavailable).
    // Chat ids only, on this phone only, in the no-backup folder; nothing about what was said.
    private val pinsFile = File(dir, "chat-pins-$userId.txt")
    private val pins: MutableSet<String> = pinsFile.takeIf { it.exists() }?.readLines()?.filter { it.isNotBlank() }?.toMutableSet() ?: mutableSetOf()

    init {
        // There's no "use the older protocol" any more (step 1): an earlier choice of it no longer counts.
        File(dir, "chat-older-$userId.txt").delete()
    }

    @Synchronized
    private fun forget(conversationId: String) {
        if (pins.remove(conversationId.lowercase())) {
            dir.mkdirs()
            pinsFile.writeText(pins.joinToString("\n"))
        }
    }

    @Synchronized
    private fun remember(conversationId: String) {
        if (pins.add(conversationId.lowercase())) {
            dir.mkdirs()
            pinsFile.writeText(pins.joinToString("\n"))
        }
    }

    private suspend fun ready(): Pair<V3Device, ChatHistory>? {
        val device = keys.chatDevice() ?: return null
        var h = history
        if (h == null || historyDevice != device.deviceId) {
            h = EncryptedChatHistory(File(dir, "chat-$userId.bin"), device.historyKey())
            history = h
            historyDevice = device.deviceId
        }
        return device to h
    }

    /**
     * Sends [text] with device keys when this chat is on them (or can be: the contact has device keys).
     * [SendOutcome.Protocol1] and null both mean "send it the usual way". A pinned chat that can't go out on
     * device keys throws EngineException instead of falling back (never silently, 9.1 rule 5).
     */
    suspend fun send(
        conversationId: String,
        isGroup: Boolean,
        contactUserId: String,
        text: String,
        id: String? = null,
        extras: MessageExtras = MessageExtras(),
    ): SendOutcome? = lock.withLock {
        val (device, h) = ready() ?: return null
        val messageId = id ?: UUID.randomUUID().toString()
        val outcome = tagged(h, conversationId, messageId) {
            if (isGroup) sendGroupText(device, h, conversationId, text, messageId, extras = extras, community = routeFor(conversationId, true))
            else sendText(device, h, conversationId, contactUserId, text, messageId, extras = extras)
        }
        if (outcome is SendOutcome.Sent) remember(conversationId)
        outcome
    }

    /**
     * Runs a send with its posts tagged as [messageId]'s, so the queue can hold them; a message that went into the
     * queue (rather than to the server) is kept as "queued" and comes back that way.
     */
    private suspend fun tagged(h: ChatHistory, conversationId: String, messageId: String, send: suspend () -> SendOutcome): SendOutcome {
        val queue = outgoing ?: return send()
        val tag = tagOf(conversationId, messageId)
        val outcome = withContext(V3SendTag(tag)) { send() }
        if (outcome is SendOutcome.Sent && queue.queue.hasTag(tag)) {
            val queued = outcome.record.copy(status = QUEUED)
            if (queued.op == null) h.put(queued, replace = true)
            return SendOutcome.Sent(queued)
        }
        return outcome
    }

    /**
     * Sends what waits in the device-key queue, in order (Tor is up again). True when nothing is left; false when
     * something still can't go out, and everything after it waits too. Queued messages show as sent once they are,
     * or as failed (Retry encrypts them again) if the server refused them.
     */
    suspend fun flushQueue(): Boolean = lock.withLock {
        val queue = outgoing ?: return true
        if (queue.queue.isEmpty()) return true
        val drained = queue.drain()
        val h = history ?: runCatching { ready()?.second }.getOrNull()
        if (h != null) {
            for ((tags, status) in listOf(drained.sent to "sent", drained.refused to "failed")) {
                for (tag in tags) {
                    val (conversation, id) = tag.split('/', limit = 2).takeIf { it.size == 2 } ?: continue
                    val record = h.messages(conversation).firstOrNull { it.id == id && it.status == QUEUED } ?: continue
                    h.put(record.copy(status = status), replace = true)
                }
            }
        }
        !drained.waiting
    }

    /** How many device-key messages (and reactions, edits, deletes) wait, encrypted, for Tor. */
    fun queuedMessages(): Int = outgoing?.queue?.items()?.mapTo(HashSet()) { it.tag }?.size ?: 0

    /**
     * Whether this chat is on device keys right now: asked before uploading a file, which has to know before it's
     * sent. Null when device keys aren't available here; false means "send it the usual way"; throws like [send].
     */
    suspend fun usesDeviceKeys(conversationId: String, isGroup: Boolean, contactUserId: String): Boolean? = lock.withLock {
        val (device, h) = ready() ?: return null
        if (isGroup) groupOnDeviceKeys(device, h, conversationId, routeFor(conversationId, true)) else contactOnDeviceKeys(device, h, conversationId, contactUserId)
    }

    /** Which protocol this one-to-one chat uses: "signal", "olm", "mixed", or null (no device keys, or not on them here). */
    suspend fun protocol(conversationId: String, contactUserId: String): String? = lock.withLock {
        val (device, _) = ready() ?: return null
        runCatching { chatProtocol(device, conversationId, contactUserId) }.getOrNull()
    }

    /** Sends a file already uploaded and released. Same answers as [send]; pass a failed send's [id] to retry it. */
    suspend fun sendFile(
        conversationId: String,
        isGroup: Boolean,
        contactUserId: String,
        file: FileDescriptor,
        id: String? = null,
        extras: MessageExtras = MessageExtras(),
    ): SendOutcome? = lock.withLock {
        val (device, h) = ready() ?: return null
        val messageId = id ?: UUID.randomUUID().toString()
        val outcome = tagged(h, conversationId, messageId) { sendChatFile(device, h, conversationId, isGroup, contactUserId, file, messageId, extras = extras, community = routeFor(conversationId, isGroup)) }
        if (outcome is SendOutcome.Sent) remember(conversationId)
        outcome
    }

    /**
     * [sendFile] for a file released just for this send. If it doesn't go out and there's no failed record to retry
     * it from, nothing will ever refer to the upload, so it's deleted (best effort: [deleteUpload] failing changes
     * nothing, and the original problem is what's reported). A failed record keeps its upload for Retry.
     */
    suspend fun sendUploaded(
        conversationId: String,
        isGroup: Boolean,
        contactUserId: String,
        file: FileDescriptor,
        id: String = UUID.randomUUID().toString(),
        extras: MessageExtras = MessageExtras(),
        deleteUpload: suspend (objectId: String) -> Unit,
    ): SendOutcome? {
        val outcome = try {
            sendFile(conversationId, isGroup, contactUserId, file, id, extras)
        } catch (e: EngineException) {
            dropUnsent(id, file, deleteUpload)
            throw e
        } catch (e: ApiException) {
            dropUnsent(id, file, deleteUpload)
            throw e
        }
        if (outcome !is SendOutcome.Sent) dropUnsent(id, file, deleteUpload)
        return outcome
    }

    private suspend fun dropUnsent(id: String, file: FileDescriptor, deleteUpload: suspend (String) -> Unit) {
        if (failed(id) != null) return
        try {
            deleteUpload(file.objectId)
        } catch (e: CancellationException) {
            throw e
        } catch (_: Exception) {
            // Best effort.
        }
    }

    /**
     * Forwards a file into this chat (part A): [reupload] encrypts and uploads it again under a new key, and the copy is
     * sent marked as forwarded. A descriptor is never reused in another chat, so a copy with the same object or key as
     * [original] is refused before anything is sent. Answers as [sendUploaded].
     */
    suspend fun forwardFile(
        conversationId: String,
        isGroup: Boolean,
        contactUserId: String,
        original: FileDescriptor,
        exp: Long? = null,
        reupload: suspend () -> FileDescriptor,
        deleteUpload: suspend (objectId: String) -> Unit,
    ): SendOutcome? {
        val copy = reupload()
        if (copy.objectId == original.objectId || copy.fileKey == original.fileKey) {
            throw EngineException("file_reused", "a forwarded file has to be uploaded again with a new key")
        }
        return sendUploaded(conversationId, isGroup, contactUserId, copy, extras = MessageExtras(forwarded = true, exp = exp), deleteUpload = deleteUpload)
    }

    /** Sends a poll (part A). Same answers as [send]; pass a failed send's [id] to retry it. */
    suspend fun sendPoll(
        conversationId: String,
        isGroup: Boolean,
        contactUserId: String,
        question: String,
        options: List<String>,
        multi: Boolean,
        exp: Long? = null,
        id: String? = null,
    ): SendOutcome? = lock.withLock {
        val (device, h) = ready() ?: return null
        val messageId = id ?: UUID.randomUUID().toString()
        val outcome = tagged(h, conversationId, messageId) {
            sendChatPoll(device, h, conversationId, isGroup, contactUserId, question, ChatPoll(options, multi), messageId, exp = exp, community = routeFor(conversationId, isGroup))
        }
        if (outcome is SendOutcome.Sent) remember(conversationId)
        outcome
    }

    /** Sends a failed message again with its id, whatever it was (text, file or poll), with its reply, forward mark and time. */
    suspend fun retry(record: ChatRecord, isGroup: Boolean, contactUserId: String): SendOutcome? {
        val extras = MessageExtras(record.reply, record.forwarded, record.exp, record.storyReply)
        val poll = record.poll
        val file = record.file
        return when {
            poll != null -> sendPoll(record.conversationId, isGroup, contactUserId, record.text, poll.options, poll.multi, record.exp, record.id)
            file != null -> sendFile(record.conversationId, isGroup, contactUserId, file, record.id, extras)
            else -> send(record.conversationId, isGroup, contactUserId, record.text, record.id, extras)
        }
    }

    /**
     * Sends the receipts this chat still owes (part A): "delivered" for every message stored here from someone else,
     * "read" for those in [onScreen] (only pass them when the person allows read receipts). One receipt per sender and
     * status, at most 100 ids each, at most once every 2 s per chat; to the sender's devices and this account's other
     * devices only. None for receipts, votes, pins or ends. Returns how many receipts went out (or into the Tor queue).
     * A receipt that can't go out isn't kept: the next pass sends it.
     */
    suspend fun sendReceipts(conversationId: String, onScreen: Collection<String> = emptyList(), now: Long = System.currentTimeMillis()): Int = lock.withLock {
        val conversation = conversationId.lowercase()
        val (device, h) = ready() ?: return 0
        val last = receiptsAt[conversation]
        if (last != null && now - last < RECEIPT_INTERVAL_MS) {
            receiptsWaiting += conversation
            return 0
        }
        val due = owedReceipts(h.messages(conversation), device.userId, onScreen.mapTo(HashSet()) { it.lowercase() }, now)
        receiptsWaiting -= conversation
        if (due.isEmpty()) return 0
        receiptsAt[conversation] = now
        var sent = 0
        for ((key, ids) in due) {
            val (author, status) = key
            val receiptId = UUID.randomUUID().toString()
            try {
                tagged(h, conversation, receiptId) {
                    sendChatReceipt(device, h, conversation, author, ids.take(ChatLimits.MAX_RECEIPT_TARGETS), status, receiptId, now)
                    SendOutcome.Protocol1
                }
                sent++
                if (ids.size > ChatLimits.MAX_RECEIPT_TARGETS) receiptsWaiting += conversation
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                // The next pass tries again; a receipt is never worth an error in the chat.
                receiptsWaiting += conversation
            }
        }
        sent
    }

    /** Chats whose receipts had to wait (the 2 s spacing, more than 100 ids, or a failure): the next pass sends them. */
    fun receiptsWaiting(): Set<String> = receiptsWaiting.toSet()

    /** Sends a reaction, edit or delete of a device-key message. These can't go the usual way, so anything else throws. */
    suspend fun sendOp(conversationId: String, isGroup: Boolean, contactUserId: String, op: ChatOp): Unit = lock.withLock {
        val (device, h) = ready() ?: throw EngineException("v3_unavailable", "device keys aren't available on this phone right now")
        val opId = UUID.randomUUID().toString()
        tagged(h, conversationId, opId) {
            sendChatOp(device, h, conversationId, isGroup, contactUserId, op, opId, community = routeFor(conversationId, isGroup))
            SendOutcome.Protocol1
        }
    }

    /** Reads this device's mailbox (9.6). Null when the chat path isn't active. Call messages go to [callEnvelopes]. */
    suspend fun receive(): ReceiveReport? = lock.withLock {
        val (device, h) = ready() ?: return null
        val report = receiveMessages(device, h, communityOf = ::community, stories = stories.sinkFor(device))
        report.received.forEach { remember(it.conversationId) }
        report.calls.forEach(callEnvelopes::offer)
        report
    }

    // --- Part B: calls over device keys (docs/signal-integration/09-part-b-calls.md) ---

    /** Call setup and room keys the mailbox delivered, waiting for their call. Memory only. */
    val callEnvelopes = CallEnvelopeHub()

    /**
     * Whether a one-to-one call in this chat goes over device keys: the chat's own rule (step 1). Null when device
     * keys aren't available on this phone (the call goes the usual way); throws like [send] for a pinned chat whose
     * contact has no devices.
     */
    suspend fun callsOnDeviceKeys(conversationId: String, contactUserId: String): Boolean? =
        inCallTime { usesDeviceKeys(conversationId, false, contactUserId) }

    /** [callsOnDeviceKeys] for a group call: every member has a signed device list (or the group is pinned). */
    suspend fun groupCallsOnDeviceKeys(groupId: String): Boolean? =
        inCallTime { usesDeviceKeys(groupId, true, "") }

    /**
     * Sends one step of a call (part B) to [contactUserId]'s devices, or to [onlyDevice] alone. It goes now or not at
     * all: never into the Tor queue, never kept, and it fails after 30 s (the call then fails as it would offline).
     */
    suspend fun sendCall(conversationId: String, contactUserId: String, call: CallMessage, onlyDevice: String? = null) {
        inCallTime {
            lock.withLock {
                val (device, _) = ready() ?: throw EngineException("v3_unavailable", "device keys aren't available on this phone right now")
                // No V3SendTag: the queueing API passes an untagged post straight through, so nothing waits for Tor.
                sendCallMessage(device, conversationId, contactUserId, call, onlyDevice)
            }
        }
    }

    /** [block] within [CALL_SEND_TIMEOUT_MS]; running out is a refusal (EngineException), not a cancellation. */
    private suspend fun <T> inCallTime(block: suspend () -> T): T = try {
        withTimeout(CALL_SEND_TIMEOUT_MS) { block() }
    } catch (e: TimeoutCancellationException) {
        throw EngineException("call_send_timeout", "the call's setup couldn't be sent in time")
    }

    /** Sends a group call's room key to the devices of [participants] (part B). Same rules as [sendCall]. */
    suspend fun sendCallKey(groupId: String, participants: List<String>, key: CallKeyMessage) {
        inCallTime {
            lock.withLock {
                val (device, _) = ready() ?: throw EngineException("v3_unavailable", "device keys aren't available on this phone right now")
                sendCallKeyMessage(device, groupId, participants, key)
            }
        }
    }

    /** This chat's stored v3 records, oldest first, the hidden reactions, edits and deletes among them. */
    fun records(conversationId: String): List<ChatRecord> = history?.messages(conversationId.lowercase()).orEmpty()

    /**
     * What the chat shows at [now], oldest first: [records] with their reactions, edits, deletes, votes, pins and
     * receipts worked in, and without what has disappeared (erased from this phone here, if the time just came).
     */
    fun shown(conversationId: String, now: Long = System.currentTimeMillis()): List<ShownMessage> {
        history?.eraseDeleted(conversationId.lowercase(), now)
        return foldChat(records(conversationId), now)
    }

    /** Erases every message whose disappearing time is up at [now], in every chat (part A). */
    fun eraseExpired(now: Long = System.currentTimeMillis()) {
        val h = history ?: return
        h.conversations().forEach { h.eraseDeleted(it, now) }
    }

    /** Whether this chat has used device keys (sent or received) on this phone. */
    fun pinned(conversationId: String): Boolean =
        conversationId.lowercase() in pins || history?.pinned(conversationId.lowercase()) == true || community(conversationId) != null

    /**
     * Why [what] can't go out in this chat, or null if it can. Device keys can't carry it yet (9.9), and a chat that
     * has used them never goes back to protocol 1 (9.1 rule 5), so this is a refusal. The screens hide these actions.
     */
    fun olderProtocolRefusal(conversationId: String, what: OlderProtocolOnly): String? =
        if (pinned(conversationId)) what.refusal else null

    /** This account has device keys but this phone isn't linked to them (see DeviceKeysSession). */
    suspend fun accountUsesKeysWithoutThisPhone(): Boolean = keys.accountUsesKeysWithoutThisPhone()

    /** A send that failed after it was encrypted, if [id] is one: it can be retried (same id) or discarded. */
    fun failed(id: String): ChatRecord? = history?.failed()?.firstOrNull { it.id == id.lowercase() }

    /** Drops a failed send the person gave up on. */
    fun discard(conversationId: String, id: String) {
        history?.remove(conversationId.lowercase(), id.lowercase())
    }

    /**
     * Unpins this chat: tests only, nothing in the app calls it (there's no way back to protocol 1, step 1). The next
     * send picks again by the rule: device keys while every member has a device list.
     */
    internal fun useOlderProtocol(conversationId: String) {
        forget(conversationId)
        history?.unpin(conversationId.lowercase())
    }

    /** Messages received on device keys and not yet seen in an open thread. */
    fun unread(conversationId: String): Int = history?.unread(conversationId.lowercase()) ?: 0

    fun markRead(conversationId: String) {
        history?.markRead(conversationId.lowercase())
    }

    fun close() {
        history = null
        stories.close()
        callEnvelopes.clear()
        outgoing?.queue?.close()
    }

    // When each chat last sent receipts (memory only).
    private val receiptsAt = java.util.concurrent.ConcurrentHashMap<String, Long>()
    private val receiptsWaiting: MutableSet<String> = java.util.concurrent.ConcurrentHashMap.newKeySet()

    companion object {
        /** A sent message still in the device-key queue (encrypted, not yet on the server). */
        const val QUEUED = "queued"

        /** At most one round of receipts per chat this often (part A). */
        const val RECEIPT_INTERVAL_MS = 2_000L

        /** A call message that can't go out within this fails the call instead of waiting (part B). */
        const val CALL_SEND_TIMEOUT_MS = 30_000L

        /**
         * The receipts [self] still owes in a chat's [records], by (sender, status): "read" for the messages in
         * [onScreen] not yet marked read, "delivered" for every other message from someone else not yet acknowledged.
         * What this account already sent (from any of its devices) counts, so nothing is sent twice.
         */
        internal fun owedReceipts(records: List<ChatRecord>, self: String, onScreen: Set<String>, now: Long): Map<Pair<String, String>, List<String>> {
            val sentRead = HashSet<String>()
            val sentAny = HashSet<String>()
            for (r in records) {
                val op = r.op ?: continue
                if (op.t != ChatOp.RECEIPT || r.fromUserId != self) continue
                sentAny += op.about
                if (op.status == ChatOp.READ) sentRead += op.about
            }
            val shown = foldChat(records, now).filter { !it.deleted }.mapTo(HashSet()) { it.record.id }
            val out = LinkedHashMap<Pair<String, String>, MutableList<String>>()
            for (m in records) {
                if (m.op != null || m.fromUserId == self || m.status != "received" || m.id !in shown) continue
                val status = when {
                    m.id in onScreen && m.id !in sentRead -> ChatOp.READ
                    m.id !in sentAny -> ChatOp.DELIVERED
                    else -> continue
                }
                out.getOrPut(m.fromUserId to status) { ArrayList() } += m.id
            }
            return out
        }

        fun tagOf(conversationId: String, messageId: String): String = conversationId.lowercase() + "/" + messageId.lowercase()
    }
}
