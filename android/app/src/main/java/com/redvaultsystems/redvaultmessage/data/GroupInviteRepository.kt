package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.AeadCiphertext
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.CryptoException
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.GroupInviteCrypto
import com.redvaultsystems.redvaultmessage.crypto.GroupInviteLink
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.ApproveJoinRequest
import com.redvaultsystems.redvaultmessage.data.api.GroupInviteApi
import com.redvaultsystems.redvaultmessage.data.api.GroupInviteApprovalRequest
import com.redvaultsystems.redvaultmessage.data.api.GroupInviteLinkDto
import com.redvaultsystems.redvaultmessage.data.api.GroupInviteLinkInput
import com.redvaultsystems.redvaultmessage.data.api.InviteAuthRequest
import com.redvaultsystems.redvaultmessage.data.api.JoinGroupInviteRequest
import com.redvaultsystems.redvaultmessage.data.api.JoinGroupInviteResult
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.model.ConversationMeta
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.withContext

/** The group's link as a member sees it. [url] is null when its secret doesn't open with this device's group key. */
class InviteLinkView(val id: String, val approval: Boolean, val secret: ByteArray?, val url: String?)

class JoinRequestView(val userId: String, val handle: String, val avatarUrl: String?)

/** What someone opening a link sees before joining. */
class InvitePreview(
    val linkId: String,
    val conversationId: String,
    val name: String,
    val memberCount: Int,
    val approval: Boolean,
    /** The group key, when the link carries it (no approval needed). */
    val groupKey: ByteArray?,
    val secret: ByteArray,
    val member: Boolean,
    val requested: Boolean,
)

/**
 * Port of the invite-link actions in the web's screens/GroupInviteLink.tsx,
 * screens/JoinGroup.tsx and state/actions.ts (approveJoinRequest,
 * joinGroupFromInvite). Everything the server gets is ciphertext, plus the
 * auth token the link's secret derives.
 */
class GroupInviteRepository(
    private val api: GroupInviteApi,
    private val users: ApiService,
    private val crypto: GroupInviteCrypto,
    private val e2e: E2eCrypto,
    private val webAppBase: String,
    /** Group key epochs and key notices; without it, the key this repository was handed is used as it is. */
    private val keys: MessageKeys? = null,
) {
    /** The group's key now. Throws with the reason when it was refused: a link can't hand out a key nobody verified. */
    private fun groupKeyOf(meta: ConversationMeta): ByteArray = keys?.currentKey(meta.id, meta.conversationKey)?.first ?: meta.conversationKey

    /** Any member: the group's link, or null when it has none. */
    suspend fun link(meta: ConversationMeta): InviteLinkView? {
        val link = apiCall(ApiClient.json) { api.getLink(meta.id) }.link ?: return null
        return view(link, groupKeyOf(meta))
    }

    /** Owner/admin: a new link (turning it on, or a reset), keeping [approval]. */
    suspend fun newLink(meta: ConversationMeta, approval: Boolean): InviteLinkView {
        val key = groupKeyOf(meta)
        val input = withContext(Dispatchers.Default) {
            val made = crypto.newLink(key, meta.handle, approval).material
            GroupInviteLinkInput(
                auth = made.auth,
                secretCiphertext = Base64Codec.encode(made.secretSealed.ciphertext),
                secretNonce = Base64Codec.encode(made.secretSealed.nonce),
                previewCiphertext = Base64Codec.encode(made.preview.ciphertext),
                previewNonce = Base64Codec.encode(made.preview.nonce),
                wrappedKey = made.wrappedKey?.let { Base64Codec.encode(it.ciphertext) },
                wrappedKeyNonce = made.wrappedKey?.let { Base64Codec.encode(it.nonce) },
            )
        }
        val link = apiCall(ApiClient.json) { api.setLink(meta.id, input) }.link ?: throw UserFacingException("Couldn't create the link.")
        return view(link, key)
    }

    /** Owner/admin: turning approval off hands the server the group key wrapped under the link's key. */
    suspend fun setApproval(meta: ConversationMeta, approval: Boolean, secret: ByteArray): InviteLinkView {
        val key = groupKeyOf(meta)
        val body = if (approval) {
            GroupInviteApprovalRequest(approval = true)
        } else {
            val wrapped = withContext(Dispatchers.Default) { crypto.wrapGroupKeyForLink(key, secret) }
            GroupInviteApprovalRequest(false, Base64Codec.encode(wrapped.ciphertext), Base64Codec.encode(wrapped.nonce))
        }
        val link = apiCall(ApiClient.json) { api.setApproval(meta.id, body) }.link ?: throw UserFacingException("Couldn't change that setting.")
        return view(link, key)
    }

    suspend fun turnOff(conversationId: String) = apiCall(ApiClient.json) { api.deleteLink(conversationId) }

    /** Owner/admin: who's waiting to join, oldest first, with their handles. */
    suspend fun requests(conversationId: String): List<JoinRequestView> {
        val pending = apiCall(ApiClient.json) { api.listRequests(conversationId) }
        return coroutineScope {
            pending.map { request ->
                async {
                    val user = runCatching { apiCall(ApiClient.json) { users.getUser(request.userId) } }.getOrNull()
                    JoinRequestView(request.userId, user?.username ?: "Unknown account", user?.avatarUrl)
                }
            }.awaitAll()
        }
    }

    /** Owner/admin: lets in someone who asked, sealing them their own copy of the group key. */
    suspend fun approve(meta: ConversationMeta, userId: String) {
        val (key, epoch) = keys?.currentKey(meta.id, meta.conversationKey) ?: (meta.conversationKey to null)
        val user = apiCall(ApiClient.json) { users.getUser(userId) }
        keys?.requireUnchangedKey(userId, user.publicKey)
        val sealed = withContext(Dispatchers.Default) {
            Base64Codec.encode(e2e.sealConversationKey(key, Base64Codec.decode(user.publicKey)))
        }
        val notice = if (keys != null && epoch != null) keys.noticeAdding(meta.id, epoch, key, userId) else null
        apiCall(ApiClient.json) { api.approve(meta.id, userId, ApproveJoinRequest(sealed, epoch, notice)) }
        // Only now: pinning before the server has it would lock the group if the approval failed.
        if (keys != null && epoch != null && notice != null) keys.adoptNotice(meta.id, epoch, key, notice)
    }

    /** An admin turning a request down, or the person who asked taking it back (their own id). */
    suspend fun deny(conversationId: String, userId: String) = apiCall(ApiClient.json) { api.deny(conversationId, userId) }

    /**
     * Opens a link someone shared: the group's name and size, and its key when no approval is needed.
     * Throws [UserFacingException] with the web's wording when the link is incomplete, reset or gone.
     */
    suspend fun open(linkId: String, secretText: String): InvitePreview {
        val secret = GroupInviteLink.secretFromFragment(secretText)
            ?: throw UserFacingException("This link is incomplete. Ask for it again, and open the whole link.")
        val auth = withContext(Dispatchers.Default) { crypto.inviteKeys(secret).auth }
        val info = try {
            apiCall(ApiClient.json) { api.open(linkId, InviteAuthRequest(auth)) }
        } catch (e: ApiException) {
            if (e.status == 401) throw e
            throw UserFacingException(if (e.status == 404) e.message ?: INVALID else "Couldn't open this link. Try again.")
        }
        val opened = withContext(Dispatchers.Default) {
            try {
                crypto.openPreview(
                    AeadCiphertext(Base64Codec.decode(info.previewCiphertext), Base64Codec.decode(info.previewNonce)),
                    if (info.wrappedKey != null && info.wrappedKeyNonce != null) {
                        AeadCiphertext(Base64Codec.decode(info.wrappedKey), Base64Codec.decode(info.wrappedKeyNonce))
                    } else {
                        null
                    },
                    secret,
                )
            } catch (e: CryptoException) {
                null
            } catch (e: IllegalArgumentException) {
                null
            }
        } ?: throw UserFacingException("This link's secret doesn't match. It may have been reset: ask the group for a new one.")
        return InvitePreview(
            linkId = linkId,
            conversationId = info.conversationId,
            name = opened.name,
            memberCount = info.memberCount,
            approval = info.approval,
            groupKey = opened.groupKey,
            secret = secret,
            member = info.member,
            requested = info.requested,
        )
    }

    /** Joins with the group key sealed to yourself (when the link carries it), or asks to join (when an admin approves). */
    suspend fun join(identity: SelfIdentity, preview: InvitePreview): JoinGroupInviteResult {
        val body = withContext(Dispatchers.Default) {
            JoinGroupInviteRequest(
                auth = crypto.inviteKeys(preview.secret).auth,
                wrappedKey = preview.groupKey?.let { Base64Codec.encode(e2e.sealConversationKey(it, identity.publicKey)) },
            )
        }
        val result = apiCall(ApiClient.json) { api.join(preview.linkId, body) }
        // Joining through a link adds this person with no other member present, so they confirm themselves: the key
        // commitment in the notice shows they really hold the group's key. It never stands in the way of the join.
        val groupKey = preview.groupKey
        if (groupKey != null && result.status == "joined") {
            try {
                confirmSelf(result.conversationId, groupKey)
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                // Everyone else's next rekey stops and says someone is unconfirmed, which is the honest answer.
            }
        }
        return result
    }

    private suspend fun confirmSelf(conversationId: String, groupKey: ByteArray) {
        val keys = keys ?: return
        val dto = apiCall(ApiClient.json) { users.listConversations() }.firstOrNull { it.id == conversationId } ?: return
        val latest = dto.keyNotice?.let { keys.checkedLatestNotice(conversationId, groupKey, it) }
        // Not one a member signed, so nothing here can be vouched for. The next rekey says this person is unconfirmed.
        if (dto.keyNotice != null && latest == null) return
        val participants = apiCall(ApiClient.json) { users.listParticipants(conversationId) }
        val members = keys.membersJoining(conversationId, participants.map { it.userId }, latest)
        keys.publishNotice(conversationId, dto.keyEpoch, groupKey, members, latest)
    }

    private suspend fun view(link: GroupInviteLinkDto, groupKey: ByteArray): InviteLinkView = withContext(Dispatchers.Default) {
        val secret = runCatching {
            crypto.openLinkSecret(Base64Codec.decode(link.secretCiphertext), Base64Codec.decode(link.secretNonce), groupKey)
        }.getOrNull()
        InviteLinkView(link.id, link.approval, secret, secret?.let { GroupInviteLink.url(webAppBase, link.id, it) })
    }

    private companion object {
        const val INVALID = "This invite link isn't valid anymore. Ask the group for a new one."
    }
}
