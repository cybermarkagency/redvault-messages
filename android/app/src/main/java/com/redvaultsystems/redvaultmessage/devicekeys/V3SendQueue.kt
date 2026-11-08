// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File
import java.io.IOException
import java.security.SecureRandom
import java.util.Base64
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec
import kotlin.coroutines.AbstractCoroutineContextElement
import kotlin.coroutines.CoroutineContext

/** One device's copy of a message, as the engine encrypted it (the body is ciphertext, never the text). */
@Serializable
data class QueuedDeviceMessage(val userId: String, val deviceId: String, val body: String)

@Serializable
data class QueuedRecipient(val userId: String, val deviceId: String)

/**
 * One device-key post waiting for Tor: exactly the request the send would have made, already encrypted.
 * [kind] "direct" is POST e2e/v3/messages ([messages]); "group" is POST e2e/v3/group-messages ([body] to [recipients]).
 */
@Serializable
data class QueuedV3Send(
    val id: String,
    /** Send order: strictly increasing, even for posts queued in the same millisecond. */
    val order: Long,
    val kind: String,
    val senderDeviceId: String,
    val conversationId: String? = null,
    val messages: List<QueuedDeviceMessage> = emptyList(),
    val body: String? = null,
    val recipients: List<QueuedRecipient> = emptyList(),
    /** The chat message it belongs to ("<conversation>/<message id>"), so the message shows as sent once all of it is. */
    val tag: String,
)

/**
 * Device-key sends made while Tor (or the server) can't be reached (docs/tor-by-default.md rule 5). It holds the
 * already-encrypted per-device payloads, so a message is encrypted once: the engine's state was saved by the
 * encryption itself (V3Device.op) before anything lands here. Sent in order once Tor is up, never another way.
 *
 * Each post is a JSON record sealed with AES-256-GCM under a random queue key, with "v3-queue:<id>" as associated data
 * so records can't be swapped. The queue key is wrapped by an Android Keystore key ([KeyWrapper], like the device's
 * storage key), so a copy of the folder alone opens nothing. In the no-backup folder, per account.
 */
class V3SendQueue(private val dir: File, private val userId: String, private val wrapper: KeyWrapper) {
    private val random = SecureRandom()
    private var key: ByteArray? = null
    private var lastOrder = 0L

    init {
        dir.mkdirs()
        lastOrder = items().maxOfOrNull { it.order } ?: 0L
    }

    private val alias get() = "redvault.v3queue." + userId.lowercase()
    private val keyAad get() = "redvault-v3-send-queue-key\u0000${userId.lowercase()}".toByteArray()
    private val keyFile get() = File(dir, "queue-key.json")

    @Serializable
    private class StoredKey(val iv: String, val wrapped: String)

    /** The queue key, made (and wrapped) on first use. Null when it can't be opened any more (the Keystore key is gone). */
    @Synchronized
    private fun key(create: Boolean): ByteArray? {
        key?.let { return it }
        if (keyFile.exists()) {
            val stored = runCatching { json.decodeFromString(StoredKey.serializer(), keyFile.readText()) }.getOrNull() ?: return null
            return runCatching {
                wrapper.unwrap(alias, KeyWrapper.Wrapped(b64d(stored.iv), b64d(stored.wrapped)), keyAad)
            }.getOrNull()?.also { key = it }
        }
        if (!create) return null
        val fresh = ByteArray(32).also(random::nextBytes)
        val wrapped = wrapper.wrap(alias, fresh, keyAad)
        writeAtomically(keyFile, json.encodeToString(StoredKey.serializer(), StoredKey(b64(wrapped.iv), b64(wrapped.ciphertext))).toByteArray())
        key = fresh
        return fresh
    }

    /** Oldest first. Records that don't open (damaged, or the key is gone) are skipped. */
    @Synchronized
    fun items(): List<QueuedV3Send> {
        val files = dir.listFiles { f -> f.name.endsWith(SUFFIX) } ?: return emptyList()
        if (files.isEmpty()) return emptyList()
        val k = key(create = false) ?: return emptyList()
        return files.mapNotNull { runCatching { read(k, it.name.removeSuffix(SUFFIX)) }.getOrNull() }.sortedBy { it.order }
    }

    @Synchronized
    fun isEmpty(): Boolean = (dir.listFiles { f -> f.name.endsWith(SUFFIX) } ?: emptyArray()).isEmpty()

    @Synchronized
    fun size(): Int = items().size

    @Synchronized
    fun hasTag(tag: String): Boolean = items().any { it.tag == tag }

    /** Seals [send] (its id and order are given here) and saves it before returning. */
    @Synchronized
    fun add(send: QueuedV3Send): QueuedV3Send {
        lastOrder = maxOf(System.currentTimeMillis() * 1000, lastOrder + 1)
        val item = send.copy(id = UUID.randomUUID().toString(), order = lastOrder)
        val k = key(create = true) ?: throw IOException("The device-key queue can't be opened")
        val plaintext = json.encodeToString(QueuedV3Send.serializer(), item).toByteArray(Charsets.UTF_8)
        val iv = ByteArray(IV_BYTES).also(random::nextBytes)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(k, "AES"), GCMParameterSpec(128, iv))
        cipher.updateAAD((AD_PREFIX + item.id).toByteArray(Charsets.UTF_8))
        val sealed = cipher.doFinal(plaintext)
        plaintext.fill(0)
        writeAtomically(File(dir, item.id + SUFFIX), iv + sealed)
        return item
    }

    /** Sent (the server stored it) or given up on. */
    @Synchronized
    fun remove(id: String) {
        File(dir, id + SUFFIX).delete()
    }

    /** The session ended: the queue key is overwritten in memory (the sealed records stay for the next sign-in). */
    @Synchronized
    fun close() {
        key?.fill(0)
        key = null
    }

    private fun read(k: ByteArray, id: String): QueuedV3Send {
        val raw = File(dir, id + SUFFIX).readBytes()
        if (raw.size < IV_BYTES + 16) throw IOException("Queue record too short")
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(k, "AES"), GCMParameterSpec(128, raw.copyOfRange(0, IV_BYTES)))
        cipher.updateAAD((AD_PREFIX + id).toByteArray(Charsets.UTF_8))
        val plaintext = cipher.doFinal(raw.copyOfRange(IV_BYTES, raw.size))
        try {
            val item = json.decodeFromString(QueuedV3Send.serializer(), String(plaintext, Charsets.UTF_8))
            if (item.id != id) throw IOException("Queue record moved")
            return item
        } finally {
            plaintext.fill(0)
        }
    }

    // Written aside and renamed: a crash mid-write never leaves a torn record.
    private fun writeAtomically(target: File, bytes: ByteArray) {
        val temp = File(target.path + ".tmp")
        temp.writeBytes(bytes)
        if (!temp.renameTo(target)) {
            target.delete()
            if (!temp.renameTo(target)) throw IOException("Couldn't save the device-key queue")
        }
    }

    private fun b64(bytes: ByteArray): String = Base64.getEncoder().encodeToString(bytes)
    private fun b64d(text: String): ByteArray = Base64.getDecoder().decode(text)

    companion object {
        private const val SUFFIX = ".q"
        private const val AD_PREFIX = "v3-queue:"
        private const val IV_BYTES = 12
        private val json = Json { ignoreUnknownKeys = true }
    }
}

/** The chat message a device-key post belongs to: only posts made under one are ever queued (chat sends, not pairing). */
class V3SendTag(val tag: String) : AbstractCoroutineContextElement(Key) {
    companion object Key : CoroutineContext.Key<V3SendTag>
}

/** Nothing reached the server, or it can't take it right now: the post waits in the queue instead of failing. */
internal fun waitsForTor(e: ApiException): Boolean = e.status == 0 || e.status == 429 || e.status >= 500

/**
 * The [V3Api] the device-key chats use (docs/tor-by-default.md): every request goes to [inner] (the Tor-routed
 * client); a chat's encrypted post that can't go out now waits in [queue] instead. While anything is queued, new
 * posts queue behind it, so they reach the server in the order they were encrypted.
 */
class QueueingV3Api(private val inner: V3Api, val queue: V3SendQueue) : V3Api by inner {
    override suspend fun sendMessages(senderDeviceId: String, conversationId: String?, messages: List<Triple<String, String, String>>) {
        val tag = currentCoroutineContext()[V3SendTag]?.tag ?: return inner.sendMessages(senderDeviceId, conversationId, messages)
        val item = QueuedV3Send(
            id = "", order = 0, kind = DIRECT, senderDeviceId = senderDeviceId, conversationId = conversationId,
            messages = messages.map { (u, d, body) -> QueuedDeviceMessage(u, d, body) }, tag = tag,
        )
        postOrQueue(item) { inner.sendMessages(senderDeviceId, conversationId, messages) }
    }

    override suspend fun sendGroupMessage(senderDeviceId: String, conversationId: String, body: String, recipients: List<Pair<String, String>>) {
        val tag = currentCoroutineContext()[V3SendTag]?.tag ?: return inner.sendGroupMessage(senderDeviceId, conversationId, body, recipients)
        val item = QueuedV3Send(
            id = "", order = 0, kind = GROUP, senderDeviceId = senderDeviceId, conversationId = conversationId,
            body = body, recipients = recipients.map { (u, d) -> QueuedRecipient(u, d) }, tag = tag,
        )
        postOrQueue(item) { inner.sendGroupMessage(senderDeviceId, conversationId, body, recipients) }
    }

    private suspend fun postOrQueue(item: QueuedV3Send, post: suspend () -> Unit) {
        if (!queue.isEmpty()) {
            queue.add(item)
            return
        }
        try {
            post()
        } catch (e: ApiException) {
            if (!waitsForTor(e)) throw e
            queue.add(item)
        }
    }

    /** What one [drain] did: tags whose posts all went out, tags the server refused, and whether anything still waits. */
    class Drained(val sent: Set<String>, val refused: Set<String>, val waiting: Boolean)

    /**
     * Sends the queue oldest first, stopping at the first post that still can't go out (Tor down, the server busy).
     * A post the server refuses (4xx) is dropped with every other queued post of the same message: that message shows
     * as failed and can be retried (encrypted again). A 401 is thrown: the session expired.
     */
    suspend fun drain(): Drained {
        val sent = LinkedHashSet<String>()
        val refused = LinkedHashSet<String>()
        for (item in queue.items()) {
            if (item.tag in refused) {
                queue.remove(item.id)
                continue
            }
            try {
                when (item.kind) {
                    DIRECT -> inner.sendMessages(item.senderDeviceId, item.conversationId, item.messages.map { Triple(it.userId, it.deviceId, it.body) })
                    GROUP -> inner.sendGroupMessage(item.senderDeviceId, item.conversationId!!, item.body!!, item.recipients.map { it.userId to it.deviceId })
                    else -> Unit
                }
                queue.remove(item.id)
                sent += item.tag
            } catch (e: CancellationException) {
                throw e
            } catch (e: ApiException) {
                if (e.status == 401) throw e
                if (waitsForTor(e)) break
                queue.remove(item.id)
                refused += item.tag
            }
        }
        val left = queue.items()
        val waitingTags = left.mapTo(HashSet()) { it.tag }
        return Drained(sent.filterTo(LinkedHashSet()) { it !in waitingTags && it !in refused }, refused, left.isNotEmpty())
    }

    companion object {
        const val DIRECT = "direct"
        const val GROUP = "group"
    }
}
