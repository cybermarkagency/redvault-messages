// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import java.io.File
import java.security.SecureRandom
import java.util.Base64
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/** A named answer as the story-key store keeps it: [userId]'s answer [rev] to [stickerId]. [payload] is the answer JSON. */
class StoredStoryAnswer(val storyId: String, val stickerId: String, val userId: String, val rev: Long, val final: Boolean, val payload: JsonObject, val ts: Long) {
    override fun toString(): String = "StoredStoryAnswer(storyId=$storyId, stickerId=$stickerId, rev=$rev)"
}

/**
 * Part D's local, encrypted story-key store (docs/signal-integration/12-part-d-stories.md): the K of each device-keys
 * story this device may open (by story and by who sent it, so only the author's counts), the named answers it was sent,
 * and the viewers an author's device still owes a key. Encrypted with AES-GCM under a key derived from the device's
 * storage key ([V3Device.storyStoreKey]), which the Android Keystore protects; on this phone only, outside backups,
 * never in a chat history. Everything about a story goes once its key is forgotten: when the story is deleted, or once
 * the server lists it nowhere (feed, archive, highlights; access revoked or blocked ends up there too). Never at its
 * `expiresAt` alone: an archived or highlighted story keeps opening.
 */
class StoryKeyStore(private val file: File, private val key: ByteArray) : StorySink {
    @Serializable
    private class Entry(val storyId: String, val senderUserId: String, val key: String, val expiresAt: Long, val receivedAt: Long)

    @Serializable
    private class Answer(val storyId: String, val stickerId: String, val userId: String, val rev: Long, val final: Boolean, val payload: JsonObject, val ts: Long)

    /** Viewers still owed a story's key (a send that didn't go out, or late friends who got access). */
    @Serializable
    private class Owed(val storyId: String, val userIds: List<String>, val expiresAt: Long)

    @Serializable
    private class Snapshot(
        val keys: List<Entry> = emptyList(),
        val answers: List<Answer> = emptyList(),
        val owed: List<Owed> = emptyList(),
    )

    private val json = Json { ignoreUnknownKeys = true }
    private val keys = LinkedHashMap<String, Entry>()
    private val answers = ArrayList<Answer>()
    private val owed = LinkedHashMap<String, Owed>()

    init {
        if (file.exists()) {
            runCatching {
                val data = file.readBytes()
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, data, 0, 12))
                cipher.updateAAD(AAD)
                val snapshot = json.decodeFromString(Snapshot.serializer(), String(cipher.doFinal(data, 12, data.size - 12), Charsets.UTF_8))
                snapshot.keys.forEach { keys[slot(it.storyId, it.senderUserId)] = it }
                answers += snapshot.answers
                snapshot.owed.forEach { owed[it.storyId] = it }
            }
        }
    }

    private fun slot(storyId: String, sender: String) = storyId.lowercase() + "|" + sender.lowercase()

    @Synchronized
    private fun save() {
        val iv = ByteArray(12).also { SecureRandom().nextBytes(it) }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, iv))
        cipher.updateAAD(AAD)
        val plain = json.encodeToString(Snapshot.serializer(), Snapshot(keys.values.toList(), answers.toList(), owed.values.toList())).toByteArray(Charsets.UTF_8)
        val sealed = cipher.doFinal(plain)
        plain.fill(0)
        file.parentFile?.mkdirs()
        val temp = File(file.parentFile, file.name + ".tmp")
        temp.writeBytes(iv + sealed)
        if (!temp.renameTo(file)) {
            file.delete()
            check(temp.renameTo(file)) { "couldn't save the story keys" }
        }
    }

    // --- Keys ---------------------------------------------------------------------------------------------------

    /**
     * A `storykey` that arrived. Kept per sender: only the one from the story's author is ever used. One past its
     * `expiresAt` is kept too (a late friend on a highlighted story gets one).
     */
    @Synchronized
    override fun storyKey(senderUserId: String, senderDeviceId: String, key: StoryKeyMessage, now: Long) {
        val at = slot(key.storyId, senderUserId)
        val encoded = Base64.getEncoder().encodeToString(key.key)
        val held = keys[at]
        if (held != null && held.key == encoded && held.expiresAt == key.expiresAt) return
        keys[at] = Entry(key.storyId.lowercase(), senderUserId.lowercase(), encoded, key.expiresAt, held?.receivedAt ?: now)
        save()
    }

    /** The author's own K for a story it just made (so this device opens it like the others). */
    @Synchronized
    fun rememberOwn(storyId: String, authorId: String, key: ByteArray, expiresAt: Long, now: Long) {
        keys[slot(storyId, authorId)] = Entry(storyId.lowercase(), authorId.lowercase(), Base64.getEncoder().encodeToString(key), expiresAt, now)
        save()
    }

    /** The K [authorId] sent for [storyId], or null (none yet, or someone else's). The caller wipes the copy. */
    @Synchronized
    fun key(storyId: String, authorId: String, @Suppress("UNUSED_PARAMETER") now: Long): ByteArray? {
        val entry = keys[slot(storyId, authorId)] ?: return null
        return runCatching { Base64.getDecoder().decode(entry.key) }.getOrNull()
    }

    /** Drops a key that didn't open its story (refused silently). */
    @Synchronized
    fun dropKey(storyId: String, authorId: String) {
        if (keys.remove(slot(storyId, authorId)) != null) save()
    }

    /** Story ids this store holds a key for. */
    @Synchronized
    fun storyIds(): Set<String> = keys.values.mapTo(HashSet()) { it.storyId }

    /** Forgets everything about [storyId]: its keys, its answers, what was owed (the story was deleted). */
    fun forget(storyId: String) = forgetAll(listOf(storyId))

    /** Forgets everything about each of [storyIds]: keys, answers, what was owed. */
    @Synchronized
    fun forgetAll(storyIds: Collection<String>) {
        val ids = storyIds.mapTo(HashSet()) { it.lowercase() }
        if (ids.isEmpty()) return
        val changed = keys.values.removeAll { it.storyId in ids } or answers.removeAll { it.storyId in ids } or owed.keys.removeAll(ids)
        if (changed) save()
    }

    /**
     * The stories with a key here that [listed] (the feed) doesn't name, as story id to the accounts that sent a key
     * for it (the author among them), leaving out keys that arrived less than [graceMs] ago: a key can come before its
     * story is published. The caller asks the archive and highlights before forgetting any. Answers and debts for a
     * story with no key here and not in the feed go now (they only matter while its key is kept).
     */
    @Synchronized
    fun unlisted(now: Long, listed: Set<String>, graceMs: Long = LISTING_GRACE_MS): Map<String, Set<String>> {
        val inFeed = listed.mapTo(HashSet()) { it.lowercase() }
        val withKey = keys.values.mapTo(HashSet()) { it.storyId }
        val strays = answers.removeAll { it.storyId !in withKey && it.storyId !in inFeed } or owed.keys.removeAll { it !in withKey && it !in inFeed }
        if (strays) save()
        return keys.values.filter { it.storyId !in inFeed && now - it.receivedAt >= graceMs }
            .groupBy({ it.storyId }, { it.senderUserId }).mapValues { it.value.toSet() }
    }

    // --- Named answers ------------------------------------------------------------------------------------------

    /**
     * A `storyanswer` that arrived. The latest revision per (story, sticker, person) counts; a final one (a quiz, a
     * question) stands once given. An answer about a story this device has no key for is kept only while it may still
     * get one: the author's device always has its own.
     */
    @Synchronized
    override fun storyAnswer(senderUserId: String, answer: StoryAnswerMessage, ts: Long, now: Long) {
        record(senderUserId, answer, ts)
    }

    /** Records [answer] by [userId] (also this account's own, as sent). False when an equal or newer one stands. */
    @Synchronized
    fun record(userId: String, answer: StoryAnswerMessage, ts: Long): Boolean {
        val story = answer.storyId.lowercase()
        val user = userId.lowercase()
        val at = answers.indexOfFirst { it.storyId == story && it.stickerId == answer.stickerId && it.userId == user }
        if (at >= 0) {
            val held = answers[at]
            if (held.final || held.rev >= answer.rev) return false
            answers[at] = Answer(story, answer.stickerId, user, answer.rev, answer.final, answer.payload, ts)
        } else {
            answers += Answer(story, answer.stickerId, user, answer.rev, answer.final, answer.payload, ts)
        }
        save()
        return true
    }

    /** The answers standing on [storyId], one per (sticker, person). */
    @Synchronized
    fun answers(storyId: String): List<StoredStoryAnswer> = answers.filter { it.storyId == storyId.lowercase() }
        .map { StoredStoryAnswer(it.storyId, it.stickerId, it.userId, it.rev, it.final, it.payload, it.ts) }

    /** [userId]'s standing answer to [stickerId] on [storyId]. */
    fun answer(storyId: String, stickerId: String, userId: String): StoredStoryAnswer? =
        answers(storyId).firstOrNull { it.stickerId == stickerId && it.userId == userId.lowercase() }

    // --- What an author's device still owes -----------------------------------------------------------------------

    @Synchronized
    fun owe(storyId: String, userIds: Collection<String>, expiresAt: Long) {
        if (userIds.isEmpty()) return
        val id = storyId.lowercase()
        val all = (owed[id]?.userIds.orEmpty() + userIds.map { it.lowercase() }).distinct()
        owed[id] = Owed(id, all, maxOf(expiresAt, owed[id]?.expiresAt ?: 0))
        save()
    }

    @Synchronized
    fun paid(storyId: String, userIds: Collection<String>) {
        val id = storyId.lowercase()
        val held = owed[id] ?: return
        val left = held.userIds - userIds.mapTo(HashSet()) { it.lowercase() }
        if (left.isEmpty()) owed.remove(id) else owed[id] = Owed(id, left, held.expiresAt)
        save()
    }

    /** Story id to the viewers still owed its key. */
    @Synchronized
    fun owed(): Map<String, List<String>> = owed.values.associate { it.storyId to it.userIds }

    companion object {
        private val AAD = "redvault-v3-story-keys-v1".toByteArray()
        /** A key for a story no route lists yet (a photo still uploading) is kept at least this long after it came (as on the desktop). */
        const val LISTING_GRACE_MS = 60 * 60_000L
    }
}
