// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonPrimitive
import java.io.File
import java.util.Base64
import java.util.UUID

/** What a key delivery did: the viewers whose devices got K (or have it waiting, encrypted, in the Tor queue), and those still owed it. */
class StoryKeyDelivery(val delivered: Set<String>, val owed: Set<String>)

/**
 * What Stories need from device keys (Part D, docs/signal-integration/12-part-d-stories.md). [V3Stories] is the real
 * one; the story code only sees this. Nothing here ever logs a key or an answer.
 */
interface StoryDeviceKeys {
    /** Whether this phone can post and open device-keys stories right now (it has trusted device keys). */
    suspend fun ready(): Boolean

    /** Whether each of [userIds] has a signed device list with at least one device (so a device-keys story can reach them). */
    suspend fun allHaveDevices(userIds: Collection<String>): Boolean

    /**
     * Sends `storykey` for [storyId] to every listed device of each of [viewers] and to this account's other devices:
     * pairwise, at most 100 copies a request, in order, through the Tor queue, a stale list fetched again once. Viewers
     * it couldn't reach are remembered as owed and tried again by [payOwed].
     */
    suspend fun deliverKey(storyId: String, key: ByteArray, expiresAt: Long, viewers: List<String>): StoryKeyDelivery

    /** Keeps this account's own K for a story it posts, before anything is sent. */
    fun rememberOwnKey(storyId: String, key: ByteArray, expiresAt: Long)

    /** The K [authorId] sent for [storyId] (a copy the caller wipes), or null while it hasn't arrived. */
    fun key(storyId: String, authorId: String): ByteArray?

    /** A key that didn't open its story: dropped, silently. */
    fun refuseKey(storyId: String, authorId: String)

    /** Forgets everything about a story (it was deleted). */
    fun forget(storyId: String)

    /**
     * The stories this phone holds a key for that aren't in [listed] (the feed just read), as story id to the accounts
     * that sent a key for it (its author among them),
     * leaving out keys that arrived in the last [StoryKeyStore.LISTING_GRACE_MS] (a story still uploading isn't listed
     * yet). Expiry alone never makes a key go: an archived or highlighted story keeps opening. The caller checks the
     * archive and highlights, then [forgetAll]s what no route lists.
     */
    fun unlisted(listed: Set<String>): Map<String, Set<String>>

    /** Forgets everything about each of [storyIds] (deleted, access revoked, or no longer listed anywhere). */
    fun forgetAll(storyIds: Collection<String>)

    /** Story id to the viewers still owed its key. */
    fun owed(): Map<String, List<String>>

    /** Remembers [viewers] as owed [storyId]'s key (late friends who just got access). */
    fun owe(storyId: String, viewers: Collection<String>, expiresAt: Long)

    /** Sends a named answer to [authorId]'s devices and this account's other devices, and keeps it as this account's. */
    suspend fun sendAnswer(authorId: String, answer: StoryAnswerMessage)

    /** The named answers standing on [storyId]: on the author's devices, everyone's; elsewhere, this account's own. */
    fun answers(storyId: String): List<StoredStoryAnswer>

    /**
     * An anonymous answer on [storyId]: [payload] sealed by the engine (`storySeal`) to each device of [authorId] whose
     * Signal certificate their verified list pins, as RVSD. A device without one is left out.
     */
    suspend fun sealAnonymous(storyId: String, authorId: String, payload: ByteArray): ByteArray

    /** Opens this device's copy of an RVSD answer (`storyUnseal`), or null (no copy for it, it doesn't open, or no Signal here). */
    fun openAnonymous(rvsd: ByteArray): ByteArray?
}

/**
 * Stories over device keys on this phone. Sends run under [lock], the chat's own, so they never interleave with a
 * chat send on the same device. Other accounts' lists, key claims and certificates are asked for under the story id,
 * which the server allows between a story's author and the accounts with access (docs/signal-integration/12-part-d-stories.md,
 * "Device lists and key claims with a story id"), so a viewer needs no one-to-one chat with the author.
 */
class V3Stories(
    private val keys: DeviceKeysSession,
    private val userId: String,
    private val dir: File,
    private val lock: Mutex,
    private val outgoing: QueueingV3Api?,
    private val clock: () -> Long = System::currentTimeMillis,
) : StoryDeviceKeys {
    @Volatile private var store: StoryKeyStore? = null
    @Volatile private var storeDevice: String? = null

    /** The store for [device], opened once per device. */
    internal fun storeFor(device: V3Device): StoryKeyStore {
        val held = store
        if (held != null && storeDevice == device.deviceId) return held
        val key = device.storyStoreKey()
        return StoryKeyStore(File(dir, "story-keys-$userId.bin"), key).also {
            store = it
            storeDevice = device.deviceId
        }
    }

    /** The store as last opened (keys are only ever read after the device opened it). */
    private fun opened(): StoryKeyStore? = store

    /** Tests: the store [ready] opened. */
    internal fun openedStore(): StoryKeyStore? = store

    private suspend fun device(): V3Device? = keys.chatDevice()

    override suspend fun ready(): Boolean = open()

    override suspend fun allHaveDevices(userIds: Collection<String>): Boolean {
        val device = device() ?: return false
        for (user in userIds.distinct()) {
            if (user == userId) continue
            val listed = try {
                verifiedDevices(device, user, null).devices
            } catch (e: EngineException) {
                return false
            } catch (e: ApiException) {
                if (e.status == 401) throw e
                return false
            }
            if (listed.isEmpty()) return false
        }
        return true
    }

    override fun rememberOwnKey(storyId: String, key: ByteArray, expiresAt: Long) {
        val s = opened() ?: throw EngineException("v3_unavailable", "device keys aren't available on this phone right now")
        s.rememberOwn(storyId, userId, key, expiresAt, clock())
    }

    override fun key(storyId: String, authorId: String): ByteArray? = opened()?.key(storyId, authorId, clock())

    override fun refuseKey(storyId: String, authorId: String) {
        opened()?.dropKey(storyId, authorId)
    }

    override fun forget(storyId: String) {
        opened()?.forget(storyId)
    }

    override fun unlisted(listed: Set<String>): Map<String, Set<String>> = opened()?.unlisted(clock(), listed).orEmpty()

    override fun forgetAll(storyIds: Collection<String>) {
        opened()?.forgetAll(storyIds)
    }

    override fun owed(): Map<String, List<String>> = opened()?.owed().orEmpty()

    override fun owe(storyId: String, viewers: Collection<String>, expiresAt: Long) {
        opened()?.owe(storyId, viewers, expiresAt)
    }

    override fun answers(storyId: String): List<StoredStoryAnswer> = opened()?.answers(storyId).orEmpty()

    // --- Sending ------------------------------------------------------------------------------------------------

    override suspend fun deliverKey(storyId: String, key: ByteArray, expiresAt: Long, viewers: List<String>): StoryKeyDelivery = lock.withLock {
        val device = device() ?: throw EngineException("v3_unavailable", "device keys aren't available on this phone right now")
        val s = storeFor(device)
        val story = storyId.lowercase()
        val others = viewers.map { it.lowercase() }.distinct().filter { it != userId }
        val envelope = encodeStoryKey(StoryKeyMessage(story, key, expiresAt), UUID.randomUUID().toString(), clock())
        val result = deliverPairwise(device, story, envelope, others, includeOwn = true)
        s.paid(story, result.delivered)
        s.owe(story, result.owed, expiresAt)
        result
    }

    override suspend fun sendAnswer(authorId: String, answer: StoryAnswerMessage) = lock.withLock {
        val device = device() ?: throw EngineException("v3_unavailable", "device keys aren't available on this phone right now")
        val s = storeFor(device)
        val now = clock()
        val envelope = encodeStoryAnswer(answer, UUID.randomUUID().toString(), now)
        val author = authorId.lowercase()
        val result = deliverPairwise(device, answer.storyId.lowercase(), envelope, if (author == userId) emptyList() else listOf(author), includeOwn = true)
        if (author != userId && author !in result.delivered) {
            throw EngineException(lastFailure ?: "contact_has_no_devices", "the answer couldn't reach the story's author")
        }
        s.record(userId, answer, now)
        Unit
    }

    @Volatile private var lastFailure: String? = null

    /**
     * Encrypts [envelope] to every listed device of [users] (and this account's other devices when [includeOwn]) and
     * posts the copies with the story id as their conversation, 100 at a time, in order. A person whose devices can't
     * be listed or encrypted to is left out (owed); a stale list is fetched again once.
     */
    private suspend fun deliverPairwise(device: V3Device, storyId: String, envelope: String, users: List<String>, includeOwn: Boolean): StoryKeyDelivery {
        var attempt = 0
        while (true) {
            attempt++
            val owed = LinkedHashSet<String>()
            val messages = ArrayList<Triple<String, String, String>>()
            for (user in users) {
                try {
                    // Under the story id: the server lets the author and an account with access read each other's
                    // list and claim each other's keys there.
                    val theirs = verifiedDevices(device, user, storyId).devices
                    if (theirs.isEmpty()) {
                        owed += user
                        lastFailure = "contact_has_no_devices"
                        continue
                    }
                    messages += encryptToDevices(device, storyId, envelope, theirs)
                } catch (e: EngineException) {
                    owed += user
                    lastFailure = e.code
                } catch (e: ApiException) {
                    if (e.status == 401) throw e
                    owed += user
                    lastFailure = e.code
                }
            }
            if (includeOwn) {
                try {
                    val mine = verifiedDevices(device, userId, null).devices.filter { it["deviceId"]?.jsonPrimitive?.contentOrNull != device.deviceId }
                    messages += encryptToDevices(device, null, envelope, mine)
                } catch (e: EngineException) {
                    // This account's other devices miss it; viewers still get theirs.
                } catch (e: ApiException) {
                    if (e.status == 401) throw e
                }
            }
            val sentTo = LinkedHashSet<String>()
            try {
                val tag = V3Chat.tagOf(storyId, UUID.randomUUID().toString())
                for (chunk in messages.chunked(ChatLimits.MAX_RECIPIENTS)) {
                    if (outgoing != null) withContext(V3SendTag(tag)) { device.api.sendMessages(device.deviceId, storyId, chunk) }
                    else device.api.sendMessages(device.deviceId, storyId, chunk)
                    chunk.forEach { sentTo += it.first }
                }
            } catch (e: ApiException) {
                if (e.status == 401) throw e
                if (e.code == "device_list_stale" && attempt == 1) continue
                lastFailure = e.code
                // What didn't go out is owed; what went out already counts.
                val delivered = users.filterTo(LinkedHashSet()) { it in sentTo && it !in owed }
                return StoryKeyDelivery(delivered, users.toSet() - delivered)
            }
            val delivered = users.filterTo(LinkedHashSet()) { it !in owed }
            return StoryKeyDelivery(delivered, owed)
        }
    }

    // --- Anonymous answers (RVSD) --------------------------------------------------------------------------------
    // The engine seals and opens the copies (`storySeal`, `storyUnseal`): no key ever leaves its state. A copy is sealed
    // against the Signal certificate the author's verified signed list pins for that device. The plaintext is the base64
    // of the answer payload (the RVSA layout, so the story and sticker ids inside still stop a server moving it), as on
    // the desktop.

    override suspend fun sealAnonymous(storyId: String, authorId: String, payload: ByteArray): ByteArray = lock.withLock {
        val device = device() ?: throw EngineException("v3_unavailable", "device keys aren't available on this phone right now")
        val story = storyId.lowercase()
        val author = authorId.lowercase()
        val plaintext = Base64.getEncoder().encodeToString(payload)
        if (plaintext.length > MAX_SEAL_PLAINTEXT) throw EngineException("answer_too_large", "that answer is too long to send without your name")
        // The author's verified list first (the engine checks each certificate against it), then the certificates.
        val listed = verifiedDevices(device, author, story).devices
        if (listed.isEmpty()) throw EngineException("contact_has_no_devices", "the story's author has no device keys listed")
        val certificates = try {
            device.api.signalCertificates(author, story).toMap()
        } catch (e: ApiException) {
            if (e.status == 401) throw e
            throw EngineException("story_seal_unavailable", "this answer can't be sealed to their devices right now")
        }
        val copies = ArrayList<RvsdCopy>()
        for (d in listed) {
            if (copies.size == Rvsd.MAX_COPIES) break
            val deviceId = d["deviceId"]?.jsonPrimitive?.contentOrNull ?: continue
            val certificate = certificates[deviceId] ?: continue
            val sealed = device.op("storySeal", "certificate" to certificate, "plaintext" to plaintext)
            if (!sealed.ok) {
                when (sealed.error) {
                    // Not pinned for that device, or not on the list this phone verified: that device gets no copy.
                    "certificate_mismatch", "unverified_device", "bad_certificate", "bad_key" -> continue
                    else -> throw EngineException(sealed.error ?: "seal_failed", "couldn't seal the answer")
                }
            }
            val bytes = sealed.json["sealed"]?.jsonPrimitive?.contentOrNull?.let { runCatching { Base64.getDecoder().decode(it) }.getOrNull() } ?: continue
            copies += RvsdCopy(deviceId, bytes)
        }
        if (copies.isEmpty()) throw EngineException("story_seal_unavailable", "none of their devices can take an answer without your name yet")
        try {
            Rvsd.encode(copies)
        } catch (e: IllegalArgumentException) {
            throw EngineException("answer_too_large", "that answer is too long to send without your name")
        }
    }

    override fun openAnonymous(rvsd: ByteArray): ByteArray? {
        val copies = Rvsd.parse(rvsd) ?: return null
        val device = openedDevice ?: return null
        val mine = copies.firstOrNull { it.deviceId == device.deviceId } ?: return null
        val opened = device.op("storyUnseal", "sealed" to Base64.getEncoder().encodeToString(mine.sealed))
        // signal_not_enabled: this device has no Signal identity, so answers can't be opened here; open_failed: not
        // sealed to this device, or changed. Either way nothing opens; the inbox counts it as unreadable.
        if (!opened.ok) return null
        val text = opened.json["plaintext"]?.jsonPrimitive?.contentOrNull ?: return null
        return runCatching { Base64.getDecoder().decode(text) }.getOrNull()
    }

    @Volatile private var openedDevice: V3Device? = null

    /** Opens this phone's device for reading answers and keys (call before [openAnonymous] or [key]); false without device keys. */
    suspend fun open(): Boolean {
        val device = device() ?: return false
        storeFor(device)
        openedDevice = device
        return true
    }

    /** The store the receive loop writes into, for [device]. */
    internal fun sinkFor(device: V3Device): StorySink {
        openedDevice = device
        return storeFor(device)
    }

    fun close() {
        store = null
        storeDevice = null
        openedDevice = null
    }

    private companion object {
        /** `storySeal` takes at most 8,192 bytes of plaintext. */
        const val MAX_SEAL_PLAINTEXT = 8 * 1024
    }
}

/** Part D: the answer JSON as a story's `storyanswer` carries it. */
fun storyAnswerPayload(json: String): JsonObject = kotlinx.serialization.json.Json.parseToJsonElement(json) as JsonObject
