// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.AddParticipantRequest
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.CreateConversationRequest
import com.redvaultsystems.redvaultmessage.data.api.FriendDto
import com.redvaultsystems.redvaultmessage.data.api.RenameRequest
import com.redvaultsystems.redvaultmessage.data.api.RoleRequest
import com.redvaultsystems.redvaultmessage.data.api.WrappedKeyForUser
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.model.ConversationMeta
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Port of the group actions in ui/src/state/actions.ts. The group key is
 * sealed separately to every member's public key, and the name is
 * encrypted with it, so the server never learns either.
 */
class GroupRepository(
    private val api: ApiService,
    private val crypto: E2eCrypto,
    /** Group key epochs (db/migrations/030). */
    private val keys: MessageKeys? = null,
) {

    suspend fun friends(): List<FriendDto> = apiCall(ApiClient.json) { api.listFriends() }

    /** Creates a group with some of your friends; returns its id. */
    suspend fun create(identity: SelfIdentity, name: String, members: List<FriendDto>): String = withContext(Dispatchers.Default) {
        val key = crypto.generateConversationKey()
        try {
            members.forEach { keys?.requireUnchangedKey(it.userId, it.publicKey) }
            val participants = listOf(WrappedKeyForUser(identity.userId, Base64Codec.encode(crypto.sealConversationKey(key, identity.publicKey)))) +
                members.map { WrappedKeyForUser(it.userId, Base64Codec.encode(crypto.sealConversationKey(key, Base64Codec.decode(it.publicKey)))) }
            val encryptedName = crypto.encryptWithConversationKey(name.trim(), key)
            apiCall(ApiClient.json) {
                api.createConversation(
                    CreateConversationRequest(
                        isGroup = true,
                        nameCiphertext = Base64Codec.encode(encryptedName.ciphertext),
                        nameNonce = Base64Codec.encode(encryptedName.nonce),
                        participants = participants,
                    ),
                )
            }.id
        } finally {
            key.fill(0)
        }
    }

    /** Owner/admin: seals the group key for a friend. They only see messages from now on. */
    suspend fun add(meta: ConversationMeta, friend: FriendDto) = withContext(Dispatchers.Default) {
        val (key, epoch) = keys?.currentKey(meta.id, meta.conversationKey) ?: (meta.conversationKey to null)
        keys?.requireUnchangedKey(friend.userId, friend.publicKey)
        val wrapped = Base64Codec.encode(crypto.sealConversationKey(key, Base64Codec.decode(friend.publicKey)))
        val notice = if (keys != null && epoch != null) keys.noticeAdding(meta.id, epoch, key, friend.userId) else null
        apiCall(ApiClient.json) { api.addParticipant(meta.id, AddParticipantRequest(friend.userId, wrapped, epoch, notice)) }
        // Only now: pinning before the server has it would lock the group if the add failed.
        if (keys != null && epoch != null && notice != null) keys.adoptNotice(meta.id, epoch, key, notice)
    }

    /**
     * Remove a member (owner/admin), or leave with your own id. Someone
     * removed may still hold the group key, so it's replaced right away
     * (when leaving, the remaining members' apps do it).
     */
    suspend fun remove(conversationId: String, userId: String, selfUserId: String? = null) {
        apiCall(ApiClient.json) { api.removeParticipant(conversationId, userId) }
        val keys = keys
        if (keys != null && userId != selfUserId) runCatching { keys.rotate(conversationId) }
    }

    suspend fun setRole(conversationId: String, userId: String, role: String) {
        apiCall(ApiClient.json) { api.setParticipantRole(conversationId, userId, RoleRequest(role)) }
    }

    suspend fun rename(meta: ConversationMeta, name: String) = withContext(Dispatchers.Default) {
        val key = keys?.currentKey(meta.id, meta.conversationKey)?.first ?: meta.conversationKey
        val encrypted = crypto.encryptWithConversationKey(name.trim(), key)
        apiCall(ApiClient.json) {
            api.renameConversation(meta.id, RenameRequest(Base64Codec.encode(encrypted.ciphertext), Base64Codec.encode(encrypted.nonce)))
        }
    }
}
