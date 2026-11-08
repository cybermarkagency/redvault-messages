// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import android.util.Log
import com.redvaultsystems.redvaultmessage.BuildConfig
import com.redvaultsystems.redvaultmessage.crypto.CommunityCrypto
import com.redvaultsystems.redvaultmessage.crypto.CommunityKeyState
import com.redvaultsystems.redvaultmessage.crypto.KeyPair
import com.redvaultsystems.redvaultmessage.crypto.KeyRecipient
import com.redvaultsystems.redvaultmessage.crypto.KeyRing
import com.redvaultsystems.redvaultmessage.crypto.SealedEpochKey
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.CommunityApi
import com.redvaultsystems.redvaultmessage.data.api.EnrollRequest
import com.redvaultsystems.redvaultmessage.data.api.MemberKeyDto
import com.redvaultsystems.redvaultmessage.data.api.RotateRequest
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Port of ui/src/lib/communityKeys.ts: the epoch keys this phone holds for each community conversation,
 * kept in memory only (like every other conversation key here), plus the upkeep any member's app does:
 * delivering the current key to members who are waiting (they paid or were let in) and, when someone
 * left, rotating to a new one. The server only says WHO is entitled; it never holds a key it can open.
 */
class CommunityKeys(private val api: CommunityApi, private val crypto: CommunityCrypto) {
    private val rings = ConcurrentHashMap<String, KeyRing>() // by conversation id
    @Volatile private var identity: SelfIdentity? = null
    private val running = AtomicBoolean(false)

    class MaintenanceResult(val rotated: Int, val enrolled: Int)

    fun setIdentity(next: SelfIdentity?) {
        if (identity?.userId != next?.userId) forgetRings()
        identity = next
    }

    /** Signing out: every epoch key is overwritten and dropped. */
    fun forget() {
        forgetRings()
        identity = null
    }

    private fun forgetRings() {
        rings.values.forEach(KeyRing::wipe)
        rings.clear()
    }

    /** The ring for a community conversation this phone has loaded, if any. */
    fun ringFor(conversationId: String): KeyRing? = rings[conversationId]

    /** Encrypts under the ring's current epoch; throws [com.redvaultsystems.redvaultmessage.crypto.EpochKeyException] without its key. */
    fun encrypt(ring: KeyRing, plaintext: String) = crypto.encrypt(ring, plaintext)

    /** Null when the epoch's key isn't held (from before joining) or the body doesn't open. */
    fun decrypt(ring: KeyRing, epoch: Long, ciphertext: ByteArray, nonce: ByteArray): String? = crypto.decrypt(ring, epoch, ciphertext, nonce)

    /** Fetches (or re-fetches) this member's sealed keys and opens them. */
    suspend fun load(communityId: String, conversationId: String): KeyRing {
        val self = identity ?: throw IllegalStateException("Not signed in")
        val dto = apiCall(ApiClient.json) { api.keys(communityId) }
        val ring = withContext(Dispatchers.Default) {
            crypto.buildKeyRing(
                communityId,
                CommunityKeyState(dto.conversationId ?: conversationId, dto.keyEpoch, dto.rekeyRequired, dto.keys.map { SealedEpochKey(it.epoch, it.wrappedKey) }),
                KeyPair(self.publicKey, self.privateKey),
            )
        }
        // The identity changed (signed out) while this was in flight: keep nothing.
        if (identity !== self) {
            ring.wipe()
            throw IllegalStateException("Not signed in")
        }
        // The ring this replaces isn't wiped: a refresh may still be decrypting with it.
        rings[ring.conversationId] = ring
        return ring
    }

    /** The ring, refreshed if the conversation list says the community has moved on to a newer epoch. */
    suspend fun ensure(communityId: String, conversationId: String, keyEpoch: Long, rekeyRequired: Boolean): KeyRing {
        val known = rings[conversationId]
        if (known != null && known.currentEpoch == keyEpoch && known.rekeyRequired == rekeyRequired && known.keys.containsKey(keyEpoch)) return known
        return load(communityId, conversationId)
    }

    /** Seals a new epoch key to everyone still in and installs it. Throws [ApiException] on a race (stale epoch, member set changed). */
    suspend fun rotate(communityId: String, conversationId: String): KeyRing {
        val ring = load(communityId, conversationId)
        val roster = apiCall(ApiClient.json) { api.roster(communityId) }
        if (!roster.rekeyRequired && roster.keyEpoch != ring.currentEpoch) return load(communityId, conversationId)
        val prepared = withContext(Dispatchers.Default) {
            crypto.prepareRotation(ring, roster.keyEpoch, roster.members.map { KeyRecipient(it.userId, it.publicKey) })
        }
        try {
            apiCall(ApiClient.json) { api.rotate(communityId, RotateRequest(prepared.fromEpoch, prepared.keys.map { MemberKeyDto(it.userId, it.wrappedKey) })) }
        } finally {
            // Our own sealed copy comes back with the keys; this one isn't kept.
            prepared.key.fill(0)
        }
        return load(communityId, conversationId)
    }

    private suspend fun enrollWaiting(communityId: String, conversationId: String): Int {
        val ring = load(communityId, conversationId)
        if (ring.rekeyRequired || !ring.keys.containsKey(ring.currentEpoch)) return 0
        val pending = apiCall(ApiClient.json) { api.enrollments(communityId) }
        val enrollments = withContext(Dispatchers.Default) { crypto.prepareEnrollments(ring, pending.map { KeyRecipient(it.userId, it.publicKey) }) }
        var enrolled = 0
        for (e in enrollments) {
            try {
                apiCall(ApiClient.json) { api.enroll(communityId, EnrollRequest(e.userId, e.epoch, e.wrappedKey)) }
                enrolled++
            } catch (err: ApiException) {
                // Someone else's app got there first, or the epoch just moved: the next round sorts it out.
                if (err.status != 409) throw err
            }
        }
        return enrolled
    }

    /**
     * One round of upkeep for every community this member is in. Safe to run from several members' apps at
     * once: the server takes the first rotation or enrollment and refuses the rest with a 409.
     */
    suspend fun runMaintenance(): MaintenanceResult {
        if (identity == null || !running.compareAndSet(false, true)) return MaintenanceResult(0, 0)
        var rotated = 0
        var enrolled = 0
        try {
            val tasks = apiCall(ApiClient.json) { api.keyTasks() }
            for (task in tasks) {
                try {
                    if (task.rekeyRequired) {
                        if (!task.canPost) continue
                        rotate(task.communityId, task.conversationId)
                        rotated++
                    }
                    if (task.pendingEnrollments > 0) enrolled += enrollWaiting(task.communityId, task.conversationId)
                } catch (e: CancellationException) {
                    throw e
                } catch (e: ApiException) {
                    if (e.status == 401) throw e
                    // A 409 is usually another member's app getting there first; the next round sorts it out.
                    if (BuildConfig.DEBUG) Log.w(TAG, "Community key upkeep failed: ${e.status} ${e.code}")
                } catch (e: Exception) {
                    if (BuildConfig.DEBUG) Log.w(TAG, "Community key upkeep failed: ${e.javaClass.name}")
                }
            }
        } finally {
            running.set(false)
        }
        return MaintenanceResult(rotated, enrolled)
    }

    private companion object {
        const val TAG = "CommunityKeys"
    }
}
