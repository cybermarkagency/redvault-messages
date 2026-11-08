// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
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
    TorRouting("this chat uses device keys, which don't work with Tor routing yet. Nothing was sent."),
}

/**
 * The chat screens' door to the v3 chat path (docs/multi-device-threat-model.md section 9). DEVELOPMENT ONLY:
 * active only when this build has the engine, the server has protocol 3 on and this phone is a trusted
 * device; otherwise every call says "not mine" and chats stay on the usual encryption.
 * History is kept per device, encrypted under a key derived from its storage key, never uploaded.
 */
class V3Chat(private val keys: DeviceKeysSession, private val userId: String, private val dir: File) {
    private val lock = Mutex()
    @Volatile private var history: ChatHistory? = null
    @Volatile private var historyDevice: String? = null

    // Which chats have used device keys, kept beside (not inside) the encrypted history so a chat stays pinned
    // even while the history can't be opened (offline at start, the server's switch off, the key unavailable).
    // Chat ids only, on this phone only, in the no-backup folder; nothing about what was said.
    private val pinsFile = File(dir, "chat-pins-$userId.txt")
    private val pins: MutableSet<String> = pinsFile.takeIf { it.exists() }?.readLines()?.filter { it.isNotBlank() }?.toMutableSet() ?: mutableSetOf()

    // Chats the person chose to send the older way ("use the older protocol"): device keys never send there again.
    private val olderFile = File(dir, "chat-older-$userId.txt")
    private val older: MutableSet<String> = olderFile.takeIf { it.exists() }?.readLines()?.filter { it.isNotBlank() }?.toMutableSet() ?: mutableSetOf()

    @Synchronized
    private fun forget(conversationId: String) {
        val id = conversationId.lowercase()
        dir.mkdirs()
        if (pins.remove(id)) pinsFile.writeText(pins.joinToString("\n"))
        if (older.add(id)) olderFile.writeText(older.joinToString("\n"))
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
        if (conversationId.lowercase() in older) return null
        val (device, h) = ready() ?: return null
        val outcome = when {
            isGroup && id != null -> sendGroupText(device, h, conversationId, text, id)
            isGroup -> sendGroupText(device, h, conversationId, text)
            id != null -> sendText(device, h, conversationId, contactUserId, text, id)
            else -> sendText(device, h, conversationId, contactUserId, text)
        }
        if (outcome is SendOutcome.Sent) remember(conversationId)
        outcome
    }

    /**
     * Whether this chat is on device keys right now: asked before uploading a file, which has to know before it's
     * sent. Null when device keys aren't available here; false means "send it the usual way"; throws like [send].
     */
    suspend fun usesDeviceKeys(conversationId: String, isGroup: Boolean, contactUserId: String): Boolean? = lock.withLock {
        if (conversationId.lowercase() in older) return null
        val (device, h) = ready() ?: return null
        isGroup || contactOnDeviceKeys(device, h, conversationId, contactUserId)
    }

    /** Which protocol this one-to-one chat uses: "signal", "olm", "mixed", or null (no device keys, or not on them here). */
    suspend fun protocol(conversationId: String, contactUserId: String): String? = lock.withLock {
        if (conversationId.lowercase() in older) return null
        val (device, _) = ready() ?: return null
        runCatching { chatProtocol(device, conversationId, contactUserId) }.getOrNull()
    }

    /** Sends a file already uploaded and released. Same answers as [send]; pass a failed send's [id] to retry it. */
    suspend fun sendFile(conversationId: String, isGroup: Boolean, contactUserId: String, file: FileDescriptor, id: String? = null): SendOutcome? = lock.withLock {
        if (conversationId.lowercase() in older) return null
        val (device, h) = ready() ?: return null
        val outcome = sendChatFile(device, h, conversationId, isGroup, contactUserId, file, id ?: UUID.randomUUID().toString())
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
        deleteUpload: suspend (objectId: String) -> Unit,
    ): SendOutcome? {
        val id = UUID.randomUUID().toString()
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
        if (conversationId.lowercase() in older) throw EngineException("v3_unavailable", "this chat now uses the older protocol, so its device-key messages can't be changed")
        val (device, h) = ready() ?: throw EngineException("v3_unavailable", "device keys aren't available on this phone right now")
        sendChatOp(device, h, conversationId, isGroup, contactUserId, op)
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
        conversationId.lowercase() !in older && (conversationId.lowercase() in pins || history?.pinned(conversationId.lowercase()) == true)

    /**
     * Why [what] can't go out in this chat, or null if it can. Device keys can't carry it (9.9), and a chat that has
     * used them only goes back to protocol 1 after "use the older protocol" (9.1 rule 5), so this is a refusal.
     */
    fun olderProtocolRefusal(conversationId: String, what: OlderProtocolOnly): String? =
        if (pinned(conversationId)) what.refusal else null

    /** This account has device keys but this phone isn't linked to them (see DeviceKeysSession). */
    suspend fun accountUsesKeysWithoutThisPhone(): Boolean = keys.accountUsesKeysWithoutThisPhone()

    /** The person chose the older protocol for this chat. */
    fun chosenOlder(conversationId: String): Boolean = conversationId.lowercase() in older

    /** A send that failed after it was encrypted, if [id] is one: it can be retried (same id) or discarded. */
    fun failed(id: String): ChatRecord? = history?.failed()?.firstOrNull { it.id == id.lowercase() }

    /** Drops a failed send the person gave up on. */
    fun discard(conversationId: String, id: String) {
        history?.remove(conversationId.lowercase(), id.lowercase())
    }

    /** "Use the older protocol for this chat": it stops being pinned, so the next send uses the usual encryption. */
    fun useOlderProtocol(conversationId: String) {
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
    }
}
