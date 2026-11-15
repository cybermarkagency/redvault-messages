// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import android.util.Log
import com.redvaultsystems.redvaultmessage.BuildConfig
import com.redvaultsystems.redvaultmessage.crypto.CommunityCrypto
import com.redvaultsystems.redvaultmessage.crypto.CommunityKeyState
import com.redvaultsystems.redvaultmessage.crypto.KeyPair
import com.redvaultsystems.redvaultmessage.crypto.KeyRing
import com.redvaultsystems.redvaultmessage.crypto.SealedEpochKey
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.CommunityApi
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.concurrent.ConcurrentHashMap

/**
 * The retired community epoch keys (before Part C2, docs/signal-integration/11-part-c2-communities.md), read only:
 * a member's old keys open older protocol-1 community messages, shown read only. Kept in memory only. Nothing here
 * sends, enrolls or rotates any more: community messages, rosters and membership upkeep are on device keys
 * (devicekeys/CommunityChat.kt).
 */
class CommunityKeys(private val api: CommunityApi, private val crypto: CommunityCrypto) {
    private val rings = ConcurrentHashMap<String, KeyRing>() // by conversation id
    @Volatile private var identity: SelfIdentity? = null
    // Conversations whose old keys were asked for this session (once each, whatever the answer).
    private val tried = ConcurrentHashMap.newKeySet<String>()

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
        tried.clear()
    }

    /** The ring for a community conversation this phone has loaded, if any. */
    fun ringFor(conversationId: String): KeyRing? = rings[conversationId]

    /** Null when the epoch's key isn't held (from before joining) or the body doesn't open. */
    fun decrypt(ring: KeyRing, epoch: Long, ciphertext: ByteArray, nonce: ByteArray): String? = crypto.decrypt(ring, epoch, ciphertext, nonce)

    /** Fetches this member's old sealed keys (GET /communities/:id/keys, read only) and opens them. */
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

    /** [load] once per conversation per session, for older messages; a failure (no old keys, offline) is quiet. */
    suspend fun loadOnce(communityId: String, conversationId: String) {
        if (!tried.add(conversationId)) return
        try {
            load(communityId, conversationId)
        } catch (e: CancellationException) {
            throw e
        } catch (e: ApiException) {
            if (e.status == 401) throw e
            if (BuildConfig.DEBUG) Log.w(TAG, "No older community keys: ${e.status} ${e.code}")
        } catch (e: Exception) {
            if (BuildConfig.DEBUG) Log.w(TAG, "No older community keys: ${e.javaClass.name}")
        }
    }

    private companion object {
        const val TAG = "CommunityKeys"
    }
}
