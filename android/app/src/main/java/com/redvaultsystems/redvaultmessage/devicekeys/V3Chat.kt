// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID

/** What can only go out on protocol 1 for now, and what a chat on device keys says when it refuses that. */
enum class OlderProtocolOnly(val refusal: String) {
    Reply("this chat uses device keys, which can't carry replies yet. Nothing was sent."),
    Poll("this chat uses device keys, which can't carry polls yet. Nothing was sent."),
    Vote("this chat uses device keys, which can't carry poll votes yet. Nothing was sent."),
    Forward("this chat uses device keys, which can't carry forwarded messages yet. Nothing was sent."),
    StoryReply("this chat uses device keys, which can't carry story replies yet. Nothing was sent."),
    Pin("this chat uses device keys, which can't carry pins yet. Nothing was sent."),
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
) {
    private val lock = Mutex()
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
    suspend fun send(conversationId: String, isGroup: Boolean, contactUserId: String, text: String, id: String? = null): SendOutcome? = lock.withLock {
        val (device, h) = ready() ?: return null
        val messageId = id ?: UUID.randomUUID().toString()
        val outcome = tagged(h, conversationId, messageId) {
            if (isGroup) sendGroupText(device, h, conversationId, text, messageId) else sendText(device, h, conversationId, contactUserId, text, messageId)
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
        if (isGroup) groupOnDeviceKeys(device, h, conversationId) else contactOnDeviceKeys(device, h, conversationId, contactUserId)
    }

    /** Which protocol this one-to-one chat uses: "signal", "olm", "mixed", or null (no device keys, or not on them here). */
    suspend fun protocol(conversationId: String, contactUserId: String): String? = lock.withLock {
        val (device, _) = ready() ?: return null
        runCatching { chatProtocol(device, conversationId, contactUserId) }.getOrNull()
    }

    /** Sends a file already uploaded and released. Same answers as [send]; pass a failed send's [id] to retry it. */
    suspend fun sendFile(conversationId: String, isGroup: Boolean, contactUserId: String, file: FileDescriptor, id: String? = null): SendOutcome? = lock.withLock {
        val (device, h) = ready() ?: return null
        val messageId = id ?: UUID.randomUUID().toString()
        val outcome = tagged(h, conversationId, messageId) { sendChatFile(device, h, conversationId, isGroup, contactUserId, file, messageId) }
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
        deleteUpload: suspend (objectId: String) -> Unit,
    ): SendOutcome? {
        val outcome = try {
            sendFile(conversationId, isGroup, contactUserId, file, id)
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

    /** Sends a reaction, edit or delete of a device-key message. These can't go the usual way, so anything else throws. */
    suspend fun sendOp(conversationId: String, isGroup: Boolean, contactUserId: String, op: ChatOp): Unit = lock.withLock {
        val (device, h) = ready() ?: throw EngineException("v3_unavailable", "device keys aren't available on this phone right now")
        val opId = UUID.randomUUID().toString()
        tagged(h, conversationId, opId) {
            sendChatOp(device, h, conversationId, isGroup, contactUserId, op, opId)
            SendOutcome.Protocol1
        }
    }

    /** Reads this device's mailbox (9.6). Null when the chat path isn't active. */
    suspend fun receive(): ReceiveReport? = lock.withLock {
        val (device, h) = ready() ?: return null
        val report = receiveMessages(device, h)
        report.received.forEach { remember(it.conversationId) }
        report
    }

    /** This chat's stored v3 records, oldest first, the hidden reactions, edits and deletes among them. */
    fun records(conversationId: String): List<ChatRecord> = history?.messages(conversationId.lowercase()).orEmpty()

    /** What the chat shows, oldest first: [records] with their reactions, edits and deletes worked in. */
    fun shown(conversationId: String): List<ShownMessage> = foldChat(records(conversationId))

    /** Whether this chat has used device keys (sent or received) on this phone. */
    fun pinned(conversationId: String): Boolean =
        conversationId.lowercase() in pins || history?.pinned(conversationId.lowercase()) == true

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
        outgoing?.queue?.close()
    }

    companion object {
        /** A sent message still in the device-key queue (encrypted, not yet on the server). */
        const val QUEUED = "queued"

        fun tagOf(conversationId: String, messageId: String): String = conversationId.lowercase() + "/" + messageId.lowercase()
    }
}
