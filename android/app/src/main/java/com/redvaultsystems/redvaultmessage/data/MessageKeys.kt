// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import android.content.Context
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.SigningKeyPair
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.ConversationDto
import com.redvaultsystems.redvaultmessage.data.api.KeyNoticeDto
import com.redvaultsystems.redvaultmessage.data.api.MessageDto
import com.redvaultsystems.redvaultmessage.data.api.RotateKeyRequest
import com.redvaultsystems.redvaultmessage.data.api.SigningKeyRequest
import com.redvaultsystems.redvaultmessage.data.api.WrappedKeyForUser
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.util.concurrent.ConcurrentHashMap

/** One conversation's unsealed keys by epoch (db/migrations/030). */
class KeyRing(
    val conversationId: String,
    val isGroup: Boolean,
    val currentEpoch: Int,
    val keys: Map<Int, ByteArray>,
    val rekeyRequired: Boolean,
    /** Set when the key failed a trust check ([KeyTrust]): nothing may be sent, and that key isn't in [keys]. */
    val keyProblem: String? = null,
) {
    /** Null when the key was refused. */
    val currentKey: ByteArray? get() = keys[currentEpoch]
}

/** How far a message's sender can be trusted; see [MessageKeys.checkSender]. */
enum class SenderCheck { Verified, Legacy, Unverified, Invalid }

/** Remembers the first signing key seen for each person (trust on first use). */
interface SigningKeyPins {
    /** Stores [keyB64] for [userId] unless one is already pinned; returns the pinned key. */
    fun pin(selfUserId: String, userId: String, keyB64: String): String

    /** The key pinned for [userId], if any. */
    fun pinned(selfUserId: String, userId: String): String?
}

class SigningKeyPinStore(context: Context) : SigningKeyPins {
    private val prefs = context.getSharedPreferences("signingKeyPins", Context.MODE_PRIVATE)

    override fun pin(selfUserId: String, userId: String, keyB64: String): String {
        pinned(selfUserId, userId)?.let { return it }
        prefs.edit().putString(name(selfUserId, userId), keyB64).apply()
        return keyB64
    }

    override fun pinned(selfUserId: String, userId: String): String? = prefs.getString(name(selfUserId, userId), null)

    private fun name(selfUserId: String, userId: String) = "${selfUserId.lowercase()}:${userId.lowercase()}"
}

/**
 * Port of ui/src/lib/groupKeys.ts and ui/src/lib/messageSigning.ts
 * (docs/group-keys-and-signatures.md):
 *
 * - Group key epochs. When someone leaves or is removed, the server takes no
 *   messages until a remaining member's app makes a fresh random key for the
 *   next epoch and seals it to every remaining member with crypto_box_seal.
 *   Older epochs' keys stay, so history stays readable.
 * - Message signatures. Each account signs every message with the Ed25519 key
 *   derived from its private key; readers check it against the sender's key,
 *   pinned on first sight.
 * - Key trust. No key is used until [KeyTrust] has accepted it: the rings other
 *   code sees have been checked, and a refused key isn't in them.
 *
 * Every user id is lowercased before it's looked up or pinned, here and in
 * [KeyTrust]: ids compare lowercase everywhere that matters, so an id in capitals
 * from the server would otherwise look like a new person with nothing pinned.
 */
class MessageKeys(
    private val api: ApiService,
    private val crypto: E2eCrypto,
    private val pins: SigningKeyPins,
    private val trust: KeyTrust,
) {
    private class Own(val identity: SelfIdentity, val pair: SigningKeyPair) {
        @Volatile var registered = false
        @Volatile var refused = false
    }

    private class SenderKey(val key: ByteArray?, val since: String?)

    @Volatile private var own: Own? = null
    private val rings = ConcurrentHashMap<String, KeyRing>()
    private val senderKeys = ConcurrentHashMap<String, SenderKey>()
    private val rotation = Mutex()
    private val registration = Mutex()

    /** Called when a session starts. */
    fun begin(identity: SelfIdentity) {
        if (own?.identity?.userId != identity.userId) {
            rings.clear()
            senderKeys.clear()
        }
        own = Own(identity, crypto.deriveSigningKeyPair(identity.privateKey))
    }

    /** Called when the session ends: nothing of it stays in memory. */
    fun end() {
        own?.pair?.secretKey?.fill(0)
        own = null
        rings.clear()
        senderKeys.clear()
    }

    // --- Key rings ---

    /**
     * Opens a conversation's keys, checks them and remembers the ring. Throws only if the current key can't be
     * opened. A key that fails its check is left out and [KeyRing.keyProblem] says why: a substituted key can
     * neither read what's sent nor forge what's shown.
     */
    suspend fun openRing(dto: ConversationDto, identity: SelfIdentity): KeyRing {
        val ring = if (dto.isGroup) verified(groupRing(dto, identity), dto, identity.userId) else directRing(dto, identity)
        rings[dto.id] = ring
        return ring
    }

    private fun unseal(wrappedKey: String, identity: SelfIdentity): ByteArray =
        crypto.unsealConversationKey(Base64Codec.decode(wrappedKey), identity.publicKey, identity.privateKey)

    /** A direct chat only ever has epoch 0, so nothing the server calls another epoch is even opened. */
    private fun directRing(dto: ConversationDto, identity: SelfIdentity): KeyRing {
        if (dto.keyEpoch != 0) {
            // A later epoch isn't believed. History stays readable only under the key this phone already pinned.
            val first = dto.keys.firstOrNull { it.epoch == 0 }?.let { k -> runCatching { unseal(k.wrappedKey, identity) }.getOrNull() }
                ?.takeIf { trust.matchesPin(identity.userId, dto.id, it) }
            val problem = trust.checkDirectKey(identity.userId, dto.id, dto.keyEpoch, first)
            return KeyRing(dto.id, false, 0, if (first != null) mapOf(0 to first) else emptyMap(), false, problem)
        }
        val key = unseal(dto.wrappedKey, identity)
        val problem = trust.checkDirectKey(identity.userId, dto.id, 0, key)
        return KeyRing(dto.id, false, 0, if (problem == null) mapOf(0 to key) else emptyMap(), false, problem)
    }

    private fun groupRing(dto: ConversationDto, identity: SelfIdentity): KeyRing {
        val keys = HashMap<Int, ByteArray>()
        keys[dto.keyEpoch] = unseal(dto.wrappedKey, identity)
        for (k in dto.keys) {
            // Only older keys are history. Nothing else the server lists is a key of this group.
            if (k.epoch in keys || k.epoch < 0 || k.epoch >= dto.keyEpoch) continue
            // An older key that won't open only affects that epoch's messages.
            runCatching { unseal(k.wrappedKey, identity) }.getOrNull()
                ?.takeIf { trust.matchesVerifiedEpoch(identity.userId, dto.id, k.epoch, it) }
                ?.let { keys[k.epoch] = it }
        }
        return KeyRing(dto.id, true, dto.keyEpoch, keys, dto.rekeyRequired)
    }

    /** Checks a group's current key against its signed notice, and drops the key if it doesn't hold up. */
    private suspend fun verified(ring: KeyRing, dto: ConversationDto, selfUserId: String): KeyRing {
        val notice = dto.keyNotice
        if (notice != null) {
            // They may have registered a signing key since this session last asked.
            forgetKeylessSenders(listOf(notice.issuerId))
            resolveSenderKeys(selfUserId, listOf(notice.issuerId))
        }
        val problem = trust.checkGroupKey(GroupKeyCheck(dto.id, selfUserId, ring.currentEpoch, ring.currentKey, notice, ::senderSigningKey))
            ?: return ring
        return KeyRing(ring.conversationId, true, ring.currentEpoch, ring.keys - ring.currentEpoch, ring.rekeyRequired, problem)
    }

    fun ringFor(conversationId: String): KeyRing? = rings[conversationId]

    /**
     * The key to encrypt with now and its epoch; [fallback] for a conversation not loaded yet. Throws
     * [UserFacingException] with the reason when this conversation's key was refused: nothing goes out under it.
     */
    fun currentKey(conversationId: String, fallback: ByteArray): Pair<ByteArray, Int> {
        val ring = rings[conversationId] ?: return fallback to 0
        ring.keyProblem?.let { throw UserFacingException(it) }
        val key = ring.currentKey ?: throw UserFacingException(KeyTrust.GROUP_KEY_UNOPENED)
        return key to ring.currentEpoch
    }

    /** A signed notice for this key and these members, or null until this account's signing key is registered. */
    suspend fun signNotice(conversationId: String, epoch: Int, key: ByteArray, members: Collection<String>, latest: KeyNoticeDto? = null): KeyNoticeDto? {
        val signer = ensureRegistered()?.takeIf { it.registered } ?: return null
        val self = signer.identity.userId
        val seq = trust.nextSeq(self, conversationId, epoch.toLong(), latest)
        val listed = members.map { it.lowercase() }.distinct().sorted()
        val input = E2eCrypto.keyNoticeInput(conversationId, self, epoch.toLong(), seq, listed, crypto.keyCommitment(key))
        return KeyNoticeDto(epoch.toLong(), seq, self, listed, Base64Codec.encode(crypto.signMessage(input, signer.pair.secretKey)))
    }

    /**
     * The notice for adding someone to a group. Reads the latest notice first, because naming an out-of-date list
     * would leave out whoever joined since. Null until this account's signing key is registered.
     */
    suspend fun noticeAdding(conversationId: String, epoch: Int, key: ByteArray, userId: String): KeyNoticeDto? {
        val self = own?.identity?.userId ?: throw IllegalStateException("Not signed in")
        val dto = apiCall(ApiClient.json) { api.listConversations() }.firstOrNull { it.id == conversationId && it.isGroup }
            ?: throw IllegalStateException("Not a group you're in")
        openRing(dto, own!!.identity).keyProblem?.let { throw UserFacingException(it) }
        val listed = apiCall(ApiClient.json) { api.listParticipants(conversationId) }.map { it.userId }
        return signNotice(conversationId, epoch, key, trust.membersToSign(self, conversationId, listed, listOf(userId, self), dto.keyNotice), dto.keyNotice)
    }

    /**
     * The group's latest notice, for someone who has just joined and has no pin of their own, or null if no member
     * who held this key signed it. The joiner names its members, so an unchecked list would vouch for the server's.
     */
    suspend fun checkedLatestNotice(conversationId: String, key: ByteArray, notice: KeyNoticeDto): KeyNoticeDto? {
        val self = own?.identity?.userId ?: return null
        forgetKeylessSenders(listOf(notice.issuerId))
        resolveSenderKeys(self, listOf(notice.issuerId))
        return notice.takeIf { trust.noticeSignatureOk(conversationId, it, key, ::senderSigningKey) }
    }

    /** Call before sealing anything to someone: see [KeyTrust.requireUnchangedKey]. */
    fun requireUnchangedKey(userId: String, publicKeyB64: String) {
        own?.identity?.userId?.let { trust.requireUnchangedKey(it, userId, publicKeyB64) }
    }

    /** Whether this phone has pinned this conversation's key: then it isn't a community, whatever the server says now. */
    fun isPinned(conversationId: String): Boolean {
        val self = own?.identity?.userId ?: return false
        return trust.isPinned(self, conversationId)
    }

    /** The members someone joining through a link names in their notice. */
    fun membersJoining(conversationId: String, listed: List<String>, latest: KeyNoticeDto?): List<String> {
        val self = own?.identity?.userId ?: return listed
        return trust.membersToSign(self, conversationId, listed, listOf(self), latest)
    }

    /** Pins a notice this phone published. Only after the server has it: pinning first would lock the group if the send failed. */
    fun adoptNotice(conversationId: String, epoch: Int, key: ByteArray, notice: KeyNoticeDto) {
        val self = own?.identity?.userId ?: return
        trust.checkGroupKey(GroupKeyCheck(conversationId, self, epoch, key, notice, ::senderSigningKey))
    }

    /**
     * Publishes a signed notice for the group's current key and these members: once for a group that has none yet,
     * and by someone who joined through a link. A client that loses the race has nothing to redo, and pins nothing:
     * it picks up the newer notice on its next load. Other failures are thrown, for the caller to ignore.
     */
    suspend fun publishNotice(conversationId: String, epoch: Int, key: ByteArray, members: Collection<String>, latest: KeyNoticeDto? = null) {
        val signed = signNotice(conversationId, epoch, key, members, latest) ?: return
        try {
            apiCall(ApiClient.json) { api.publishKeyNotice(conversationId, signed) }
        } catch (e: ApiException) {
            if (e.code == "stale_key_notice") return
            throw e
        }
        adoptNotice(conversationId, epoch, key, signed)
    }

    /** Re-reads one conversation's keys (someone else rotated). */
    suspend fun refresh(conversationId: String): KeyRing? {
        val identity = own?.identity ?: return null
        val dto = apiCall(ApiClient.json) { api.listConversations() }.firstOrNull { it.id == conversationId } ?: run {
            rings.remove(conversationId)
            return null
        }
        return openRing(dto, identity)
    }

    /**
     * Moves a group to its next key. Any member may; when two do at once the
     * server keeps one and the other picks up its key.
     */
    suspend fun rotate(conversationId: String) = rotation.withLock {
        repeat(MAX_ROTATION_ATTEMPTS) {
            val identity = own?.identity ?: throw IllegalStateException("Not signed in")
            val dto = apiCall(ApiClient.json) { api.listConversations() }.firstOrNull { it.id == conversationId && it.isGroup }
                ?: throw IllegalStateException("Not a group you're in")
            val ring = openRing(dto, identity)
            if (!ring.rekeyRequired) return@withLock
            ring.keyProblem?.let { throw UserFacingException(it) }
            val oldKey = ring.currentKey ?: throw UserFacingException(KeyTrust.GROUP_KEY_UNOPENED)
            val participants = apiCall(ApiClient.json) { api.listParticipants(conversationId) }
            // The server's list isn't taken on trust: the new key is only sealed to people a signed notice named.
            if (trust.unconfirmedMembers(identity.userId, conversationId, participants.map { it.userId }).isNotEmpty()) {
                throw UserFacingException(KeyTrust.UNCONFIRMED_MEMBER)
            }
            val newKey = crypto.generateConversationKey()
            try {
                val keys = participants.mapNotNull { p ->
                    val user = apiCall(ApiClient.json) { api.getUser(p.userId) }
                    if (!user.deleted) trust.requireUnchangedKey(identity.userId, p.userId, user.publicKey)
                    // A deleted account's key is gone: nothing to seal to.
                    if (user.deleted) null else WrappedKeyForUser(p.userId, Base64Codec.encode(crypto.sealConversationKey(newKey, Base64Codec.decode(user.publicKey))))
                }
                var nameCiphertext: String? = null
                var nameNonce: String? = null
                if (dto.nameCiphertext != null && dto.nameNonce != null) {
                    val name = crypto.decryptWithConversationKey(Base64Codec.decode(dto.nameCiphertext), Base64Codec.decode(dto.nameNonce), oldKey)
                    val encrypted = crypto.encryptWithConversationKey(name, newKey)
                    nameCiphertext = Base64Codec.encode(encrypted.ciphertext)
                    nameNonce = Base64Codec.encode(encrypted.nonce)
                }
                val notice = signNotice(conversationId, ring.currentEpoch + 1, newKey, keys.map { it.userId })
                val result = apiCall(ApiClient.json) {
                    api.rotateGroupKey(conversationId, RotateKeyRequest(ring.currentEpoch, keys, nameCiphertext, nameNonce, notice))
                }
                rings[conversationId] = KeyRing(conversationId, true, result.keyEpoch, ring.keys + (result.keyEpoch to newKey.copyOf()), false)
                if (notice != null) adoptNotice(conversationId, result.keyEpoch, newKey, notice)
                return@withLock
            } catch (e: ApiException) {
                // Lost a race: look again.
                if (e.code != "stale_key_epoch" && e.code != "membership_changed") throw e
            } finally {
                newKey.fill(0)
            }
        }
        throw IllegalStateException("Couldn't give this group a new key. Try again.")
    }

    // --- Signing ---

    private suspend fun ensureRegistered(): Own? {
        val signer = own ?: return null
        if (signer.registered || signer.refused) return signer
        registration.withLock {
            if (signer.registered || signer.refused) return signer
            try {
                apiCall(ApiClient.json) { api.registerSigningKey(SigningKeyRequest(Base64Codec.encode(signer.pair.publicKey))) }
                signer.registered = true
                val pinned = pins.pin(signer.identity.userId, signer.identity.userId, Base64Codec.encode(signer.pair.publicKey))
                // Our own key is known now: don't keep answering "no key" for our own signed messages.
                senderKeys[signer.identity.userId] = SenderKey(Base64Codec.decode(pinned), null)
            } catch (e: ApiException) {
                // The server holds a different key for this account (it never should): send unsigned, readers flag it.
                if (e.code == "signing_key_exists") signer.refused = true
            } catch (_: Exception) {
                // Offline or an older server: try again on the next send.
            }
        }
        return signer
    }

    /** This account's signature over a message about to be sent, or null when signing isn't set up (yet). */
    suspend fun signatureFor(conversationId: String, keyEpoch: Int, nonce: ByteArray, ciphertext: ByteArray): String? {
        val signer = ensureRegistered()?.takeIf { it.registered } ?: return null
        return runCatching {
            val input = E2eCrypto.messageSignatureInput(conversationId, signer.identity.userId, keyEpoch.toLong(), nonce, ciphertext)
            Base64Codec.encode(crypto.signMessage(input, signer.pair.secretKey))
        }.getOrNull()
    }

    /** Makes sure these senders' signing keys are known; the first one seen for each is pinned. */
    suspend fun resolveSenderKeys(selfUserId: String, userIds: Collection<String>) {
        for (userId in userIds.mapTo(HashSet()) { it.lowercase() }) {
            if (senderKeys.containsKey(userId)) continue
            // Someone who signs doesn't stop: a pinned key holds, whatever the server answers now.
            val stored = pins.pinned(selfUserId, userId)?.let(Base64Codec::decode)
            val user = runCatching { apiCall(ApiClient.json) { api.getUser(userId) } }.getOrNull()
            if (user == null) {
                if (stored != null) senderKeys[userId] = SenderKey(stored, PINNED_SINCE)
                continue
            }
            val key = stored ?: user.signingPublicKey?.let { Base64Codec.decode(pins.pin(selfUserId, userId, it)) }
            senderKeys[userId] = SenderKey(key, user.signingKeySince ?: PINNED_SINCE.takeIf { stored != null })
        }
    }

    /**
     * These people just sent signed messages. Anyone we hold no key for is looked up again by the next
     * [resolveSenderKeys]: they registered a key after we last asked. Without this, a "no key yet" answer
     * stayed cached for the whole session and their signed messages read as forged.
     */
    fun forgetKeylessSenders(userIds: Collection<String>) {
        for (id in userIds) {
            val userId = id.lowercase()
            if (senderKeys[userId]?.key == null) senderKeys.remove(userId)
        }
    }

    /**
     * The pinned signing key of someone looked up with [resolveSenderKeys], or null. This account's own needs no
     * asking: it's derived here, so what the server says about it doesn't matter.
     */
    fun senderSigningKey(userId: String): ByteArray? {
        val id = userId.lowercase()
        val signer = own
        if (signer != null && signer.identity.userId.lowercase() == id) return signer.pair.publicKey
        return senderKeys[id]?.key
    }

    /**
     * Verified: a valid signature by the sender's pinned key. Legacy: unsigned,
     * from before the sender had a key. Unverified: an unsigned message from
     * someone whose apps sign, sent after they started (an older app of theirs,
     * a stripped signature or a forgery), in a group or a direct chat: shown
     * with a warning, and it can't edit, react, vote or pin. Invalid: a
     * signature that doesn't check out.
     */
    /** [conversationId] is the chat being read: the message's own claim to one isn't believed. */
    fun checkSender(dto: MessageDto, conversationId: String): SenderCheck {
        val known = senderKeys[dto.senderId.lowercase()]
        val signature = dto.signature
        if (signature != null) {
            val key = known?.key ?: return SenderCheck.Invalid
            return try {
                val input = E2eCrypto.messageSignatureInput(
                    conversationId, dto.senderId, dto.keyEpoch.toLong(), Base64Codec.decode(dto.nonce!!), Base64Codec.decode(dto.ciphertext!!),
                )
                if (crypto.verifyMessageSignature(Base64Codec.decode(signature), input, key)) SenderCheck.Verified else SenderCheck.Invalid
            } catch (_: Exception) {
                SenderCheck.Invalid
            }
        }
        val since = known?.since
        if (known?.key == null || since == null) return SenderCheck.Legacy
        return if (dto.createdAt.take(10) >= since) SenderCheck.Unverified else SenderCheck.Legacy
    }

    private companion object {
        const val MAX_ROTATION_ATTEMPTS = 3

        /** For a pinned signer the server says nothing about: they sign, so any date counts as after they started. */
        const val PINNED_SINCE = "0000-00-00"
    }
}
