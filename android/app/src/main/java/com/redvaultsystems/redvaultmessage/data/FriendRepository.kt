// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.data.api.AcceptFriendRequest
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.FriendRequestDto
import com.redvaultsystems.redvaultmessage.data.api.UsernameRequest
import com.redvaultsystems.redvaultmessage.data.api.WrappedKeyForUser
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.withContext

/** Someone found by handle: a read-only preview, nothing is created server-side. */
data class ContactPreview(
    val userId: String,
    val handle: String,
    val avatarUrl: String?,
    val isSelf: Boolean,
    val deleted: Boolean,
)

/** A pending friend request with the other side's current handle resolved. */
data class FriendRequestView(val id: String, val userId: String, val handle: String, val avatarUrl: String?)

data class FriendRequests(val incoming: List<FriendRequestView> = emptyList(), val outgoing: List<FriendRequestView> = emptyList())

/** Port of the friends half of ui/src/state/actions.ts. */
class FriendRepository(private val api: ApiService, private val crypto: E2eCrypto, private val contactKeys: ContactKeyPins? = null) {

    suspend fun lookup(handle: String, selfUserId: String): ContactPreview {
        val found = apiCall(ApiClient.json) { api.lookupUser(handle.trim()) }
        val user = apiCall(ApiClient.json) { api.getUser(found.userId) }
        return ContactPreview(user.userId, user.username, user.avatarUrl, user.userId == selfUserId, user.deleted)
    }

    suspend fun sendRequest(handle: String) {
        apiCall(ApiClient.json) { api.sendFriendRequest(UsernameRequest(handle)) }
    }

    /** Both directions of pending requests. Requests whose other side can't be resolved any more are skipped. */
    suspend fun pending(): FriendRequests = coroutineScope {
        val incoming = async { apiCall(ApiClient.json) { api.incomingFriendRequests() } }
        val outgoing = async { apiCall(ApiClient.json) { api.outgoingFriendRequests() } }
        FriendRequests(
            incoming = hydrate(incoming.await().filter { it.status == "pending" }) { it.requesterId },
            outgoing = hydrate(outgoing.await().filter { it.status == "pending" }) { it.recipientId },
        )
    }

    /**
     * Accepts a request: generates a fresh conversation key and seals it to
     * both sides' public keys, and the server creates the friendship and the
     * chat together. Returns the new conversation's id.
     */
    suspend fun accept(identity: SelfIdentity, request: FriendRequestView): String = withContext(Dispatchers.Default) {
        val requester = apiCall(ApiClient.json) { api.getUser(request.userId) }
        if (contactKeys?.keyChanged(identity.userId, request.userId, requester.publicKey, verified = false) == true) {
            throw UserFacingException(KeyTrust.CONTACT_KEY_CHANGED)
        }
        val theirPublicKey = Base64Codec.decode(requester.publicKey)
        val conversationKey = crypto.generateConversationKey()
        try {
            val created = apiCall(ApiClient.json) {
                api.acceptFriendRequest(
                    request.id,
                    AcceptFriendRequest(
                        listOf(
                            WrappedKeyForUser(identity.userId, Base64Codec.encode(crypto.sealConversationKey(conversationKey, identity.publicKey))),
                            WrappedKeyForUser(request.userId, Base64Codec.encode(crypto.sealConversationKey(conversationKey, theirPublicKey))),
                        ),
                    ),
                )
            }
            created.id
        } finally {
            // The chat list unseals its own copy from the server on the next refresh.
            conversationKey.fill(0)
        }
    }

    /** The requester isn't told either way. */
    suspend fun deny(requestId: String) {
        apiCall(ApiClient.json) { api.denyFriendRequest(requestId) }
    }

    suspend fun cancel(requestId: String) {
        apiCall(ApiClient.json) { api.cancelFriendRequest(requestId) }
    }

    private suspend fun hydrate(dtos: List<FriendRequestDto>, otherUserId: (FriendRequestDto) -> String): List<FriendRequestView> =
        coroutineScope {
            dtos.map { dto ->
                async {
                    val userId = otherUserId(dto)
                    runCatching { apiCall(ApiClient.json) { api.getUser(userId) } }.getOrNull()
                        ?.let { FriendRequestView(dto.id, userId, it.username, it.avatarUrl) }
                }
            }.awaitAll().filterNotNull()
        }
}
