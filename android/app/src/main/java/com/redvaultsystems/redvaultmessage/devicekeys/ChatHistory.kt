// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * One message in this device's local v3 history (docs/multi-device-threat-model.md 9.7). Never uploaded.
 * [direction] is "in" or "out"; [status] is "sent", "failed" or "received". Records written before files and ops
 * existed have neither field, and read back as plain text messages.
 */
@Serializable
data class ChatRecord(
    val conversationId: String,
    val id: String,
    val fromUserId: String,
    val fromDeviceId: String,
    val ts: Long,
    val arrivedAt: Long,
    val text: String,
    val direction: String,
    val status: String,
    /** A file message: [text] is then empty. */
    val file: FileDescriptor? = null,
    /** A reaction, edit or delete: hidden from the chat, and worked into the message it targets by [foldChat]. */
    val op: ChatOp? = null,
)

/** This device's decrypted history and the per-conversation `v3` pins (9.1 rule 5). */
interface ChatHistory {
    /** Adds the record, or replaces the one with the same (conversation, id) when [replace] (a retry); false when it was a duplicate. */
    fun put(record: ChatRecord, replace: Boolean = false): Boolean
    fun has(conversationId: String, id: String): Boolean
    fun messages(conversationId: String): List<ChatRecord>
    fun pinned(conversationId: String): Boolean
    fun pin(conversationId: String)
    fun unpin(conversationId: String)

    /** Removes one message (a failed send the person discarded). */
    fun remove(conversationId: String, id: String)

    /**
     * Wipes the text and file key of messages their sender deleted, and the edits and reactions that referred to them,
     * so "delete for everyone" doesn't leave the words on this phone (9.3). Run it after every new delete or message.
     */
    fun eraseDeleted(conversationId: String)

    /** Sends that failed after they were encrypted: kept so they can be retried with the same id. */
    fun failed(): List<ChatRecord>

    /** Messages received here and not yet seen in an open thread. */
    fun unread(conversationId: String): Int

    /** Everything received in this chat so far counts as seen. */
    fun markRead(conversationId: String)
    fun wipe()
}

/**
 * History kept encrypted at rest: one AES-256-GCM file per account, under a key derived from the device's
 * storage key (V3Device.historyKey), so it is only readable while that key's Keystore wrapper works.
 * Written whole on each change (a development preview: the file is small); wiped with the device.
 */
class EncryptedChatHistory(private val file: File, private val key: ByteArray) : ChatHistory {
    @Serializable
    private class Snapshot(
        val records: List<ChatRecord> = emptyList(),
        val pins: List<String> = emptyList(),
        /** Per chat: the arrival time up to which received messages were seen. */
        val read: Map<String, Long> = emptyMap(),
    )

    private val json = Json { ignoreUnknownKeys = true }
    private val records = ArrayList<ChatRecord>()
    private val pins = LinkedHashSet<String>()
    private val read = HashMap<String, Long>()

    init {
        if (file.exists()) {
            runCatching {
                val data = file.readBytes()
                val iv = data.copyOfRange(0, 12)
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, iv))
                cipher.updateAAD(AAD)
                val snapshot = json.decodeFromString(Snapshot.serializer(), String(cipher.doFinal(data, 12, data.size - 12), Charsets.UTF_8))
                records += snapshot.records
                pins += snapshot.pins
                read += snapshot.read
            }
        }
    }

    @Synchronized
    private fun save() {
        val iv = ByteArray(12).also { SecureRandom().nextBytes(it) }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, iv))
        cipher.updateAAD(AAD)
        val sealed = cipher.doFinal(json.encodeToString(Snapshot.serializer(), Snapshot(records.toList(), pins.toList(), read.toMap())).toByteArray(Charsets.UTF_8))
        file.parentFile?.mkdirs()
        val temp = File(file.parentFile, file.name + ".tmp")
        temp.writeBytes(iv + sealed)
        if (!temp.renameTo(file)) {
            file.delete()
            check(temp.renameTo(file)) { "couldn't save the history" }
        }
    }

    @Synchronized
    override fun put(record: ChatRecord, replace: Boolean): Boolean {
        val at = records.indexOfFirst { it.conversationId == record.conversationId && it.id == record.id }
        if (at >= 0) {
            if (!replace) return false
            records[at] = record
        } else {
            records += record
        }
        save()
        return true
    }

    @Synchronized
    override fun has(conversationId: String, id: String) = records.any { it.conversationId == conversationId && it.id == id }

    @Synchronized
    override fun messages(conversationId: String) =
        records.filter { it.conversationId == conversationId }.sortedWith(compareBy({ it.ts }, { it.arrivedAt }))

    @Synchronized
    override fun pinned(conversationId: String) = conversationId in pins

    @Synchronized
    override fun pin(conversationId: String) {
        if (pins.add(conversationId)) save()
    }

    @Synchronized
    override fun unpin(conversationId: String) {
        if (pins.remove(conversationId)) save()
    }

    @Synchronized
    override fun remove(conversationId: String, id: String) {
        if (records.removeAll { it.conversationId == conversationId && it.id == id }) save()
    }

    @Synchronized
    override fun eraseDeleted(conversationId: String) {
        val plan = planErase(records.filter { it.conversationId == conversationId })
        if (plan.isEmpty) return
        records.removeAll { it.conversationId == conversationId && it.id in plan.remove }
        records.replaceAll { if (it.conversationId == conversationId && it.id in plan.blank) it.copy(text = "", file = null) else it }
        save()
    }

    @Synchronized
    override fun failed() = records.filter { it.status == "failed" }

    @Synchronized
    override fun unread(conversationId: String): Int {
        val seen = read[conversationId] ?: 0L
        return records.count { it.conversationId == conversationId && it.op == null && it.direction == "in" && it.arrivedAt > seen }
    }

    @Synchronized
    override fun markRead(conversationId: String) {
        val newest = records.filter { it.conversationId == conversationId && it.direction == "in" }.maxOfOrNull { it.arrivedAt } ?: return
        if ((read[conversationId] ?: 0L) < newest) {
            read[conversationId] = newest
            save()
        }
    }

    @Synchronized
    override fun wipe() {
        records.clear()
        pins.clear()
        read.clear()
        file.delete()
    }

    private companion object {
        val AAD = "redvault-v3-history".toByteArray()
    }
}
