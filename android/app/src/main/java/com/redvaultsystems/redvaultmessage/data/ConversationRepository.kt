package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.Commitment
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.crypto.EpochKeyException
import com.redvaultsystems.redvaultmessage.crypto.KeyRing as CommunityRing
import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.model.CommunityLink
import com.redvaultsystems.redvaultmessage.data.api.ApiService
import com.redvaultsystems.redvaultmessage.data.api.ConversationDto
import com.redvaultsystems.redvaultmessage.data.api.MarkReadRequest
import com.redvaultsystems.redvaultmessage.data.api.MessageDto
import com.redvaultsystems.redvaultmessage.data.api.SendMessageRequest
import com.redvaultsystems.redvaultmessage.data.api.VerificationDto
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import com.redvaultsystems.redvaultmessage.data.model.ConversationMeta
import com.redvaultsystems.redvaultmessage.data.model.Disappearing
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import com.redvaultsystems.redvaultmessage.data.model.GroupMemberView
import com.redvaultsystems.redvaultmessage.data.api.ParticipantDto
import kotlinx.coroutines.awaitAll
import com.redvaultsystems.redvaultmessage.data.model.EditEvent
import com.redvaultsystems.redvaultmessage.data.model.Message
import com.redvaultsystems.redvaultmessage.data.model.MessageFolding
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.data.model.NoticeKind
import com.redvaultsystems.redvaultmessage.data.model.NoticeView
import com.redvaultsystems.redvaultmessage.data.model.ParsedBody
import com.redvaultsystems.redvaultmessage.data.model.PollEvent
import com.redvaultsystems.redvaultmessage.data.model.PollView
import com.redvaultsystems.redvaultmessage.data.model.ReactionTally
import com.redvaultsystems.redvaultmessage.data.model.ReplyRef
import com.redvaultsystems.redvaultmessage.data.model.StoryReplyRef
import com.redvaultsystems.redvaultmessage.data.model.ReplyView
import com.redvaultsystems.redvaultmessage.data.model.ChannelPostRef
import com.redvaultsystems.redvaultmessage.data.model.RichMessage
import com.redvaultsystems.redvaultmessage.data.model.MessageSecurity
import com.redvaultsystems.redvaultmessage.data.model.VerificationStatus
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.time.Instant

/**
 * Every send gets a fresh nonce, so identical bytes under another id are a replay: they show once, the first time
 * they appear. Both the nonce and the ciphertext have to match: two damaged messages that share a nonce are still
 * two messages, and each shows its own error. Compared as bytes, since one value has several base64 spellings.
 *
 * The epoch counts too: each has its own key, so the same bytes under another epoch can't open (and a signature
 * covers the epoch). They can only show as an error, like any other junk, never as a readable second copy.
 */
internal fun withoutReplays(messages: List<MessageDto>): List<MessageDto> {
    fun bytes(base64: String?) = runCatching { Base64Codec.encode(Base64Codec.decode(base64.orEmpty())) }.getOrDefault(base64.orEmpty())
    val seen = HashSet<String>()
    return messages.filter { seen.add(bytes(it.nonce) + ":" + bytes(it.ciphertext) + ":" + it.keyEpoch) }
}

/** [hasOlder]: the chat has older messages than the window fetched (see [MessagePaging]). */
class MessagesResult(val messages: List<Message>, val unread: Int, val hasOlder: Boolean = false)

/** What a reply points at: [ref] travels in the message; [fromMe] is for showing it right away. */
class ReplyTarget(val ref: ReplyRef, val fromMe: Boolean)

/**
 * Port of the DM half of ui/src/state/actions.ts: unseal conversation keys,
 * fetch and decrypt messages, compute read state from receipts, send, and
 * the stage-3 commitment check. All crypto runs on Dispatchers.Default.
 */
class ConversationRepository(
    private val api: ApiService,
    private val crypto: E2eCrypto,
    private val hidden: HiddenMessagesStore? = null,
    /** Group key epochs and message signatures (docs/group-keys-and-signatures.md). */
    private val keys: MessageKeys? = null,
    /** Community epoch keys (data/CommunityKeys.kt); without it, community conversations aren't loaded. */
    private val communities: CommunityKeys? = null,
) {

    /** Includes communities only when this repository can read them (their messages use key epochs). */
    suspend fun listConversations(): List<ConversationDto> =
        apiCall(ApiClient.json) { api.listConversations(if (communities != null) "communities" else null) }

    /** Chats from people who aren't your friends, waiting for you to accept or ignore them. */
    suspend fun listRequests(): List<ConversationDto> = apiCall(ApiClient.json) { api.listConversationRequests() }

    suspend fun respondToRequest(conversationId: String, accept: Boolean) {
        apiCall(ApiClient.json) { if (accept) api.acceptConversationRequest(conversationId) else api.ignoreConversationRequest(conversationId) }
    }

    /**
     * Unseals a DM's key and resolves the other participant. Returns null if
     * it isn't a well-formed 1:1 (e.g. the other participant already left).
     */
    /**
     * Opens every key the conversation carries for this session, on each
     * load (a group's key can change at any time). Someone left or was
     * removed: gives the group a new key first (any member may).
     */
    suspend fun refreshKeys(dto: ConversationDto, identity: SelfIdentity): KeyRing? = withContext(Dispatchers.Default) {
        // A community has its own epoch keys (CommunityKeys): this ring is for ordinary chats and groups.
        if (isCommunity(dto)) return@withContext null
        val keys = keys ?: return@withContext null
        if (dto.isGroup && dto.rekeyRequired) {
            runCatching { keys.rotate(dto.id) }
            keys.ringFor(dto.id)?.takeIf { !it.rekeyRequired }?.let { return@withContext it }
        }
        keys.openRing(dto, identity)
    }

    /** Why this chat's key was refused, when it was (see [KeyTrust]): nothing can be sent there, and the chat says so. */
    fun keyProblem(conversationId: String): String? = keys?.ringFor(conversationId)?.keyProblem

    /** A chat this phone pinned isn't a community, whatever the server says now: communities skip those checks. */
    private fun isCommunity(dto: ConversationDto) = dto.community != null && keys?.isPinned(dto.id) != true

    suspend fun hydrateMeta(dto: ConversationDto, identity: SelfIdentity): ConversationMeta? = withContext(Dispatchers.Default) {
        if (isCommunity(dto)) return@withContext hydrateCommunity(dto, identity, previous = null)
        val ring = refreshKeys(dto, identity)
        // With a ring, what it holds is all there is: a refused key is never opened again here. A key of zeros opens nothing.
        val conversationKey = if (ring != null) {
            ring.currentKey ?: ByteArray(E2eCrypto.AEAD_KEY_BYTES)
        } else {
            crypto.unsealConversationKey(Base64Codec.decode(dto.wrappedKey), identity.publicKey, identity.privateKey)
        }
        val participants = apiCall(ApiClient.json) { api.listParticipants(dto.id) }
        if (dto.isGroup) {
            // A group from before key notices gets its first one here.
            if (ring != null && dto.keyNotice == null && ring.keyProblem == null) publishFirstNotice(dto.id, ring, conversationKey, participants)
            return@withContext hydrateGroup(dto, identity, conversationKey, participants)
        }
        val other = participants.firstOrNull { it.userId != identity.userId } ?: return@withContext null
        val user = apiCall(ApiClient.json) { api.getUser(other.userId) }
        val publicKey = Base64Codec.decode(user.publicKey)
        ConversationMeta(
            id = dto.id,
            otherUserId = other.userId,
            handle = user.username,
            avatarUrl = user.avatarUrl,
            otherPublicKey = publicKey,
            otherPublicKeyB64 = user.publicKey,
            conversationKey = conversationKey,
            safetyNumber = crypto.safetyNumberDigits(identity.publicKey, publicKey),
            fetchedAtMs = System.currentTimeMillis(),
        )
    }

    /** Best effort: it never stands in the way of opening the group, and the next load tries again. */
    private suspend fun publishFirstNotice(conversationId: String, ring: KeyRing, key: ByteArray, participants: List<ParticipantDto>) {
        // Only at epoch 0: a later key with no notice is something this phone can't vouch for, and members with pins
        // would believe it. The next rekey gives it a notice.
        if (ring.currentEpoch != 0) return
        try {
            keys?.publishNotice(conversationId, ring.currentEpoch, key, participants.map { it.userId })
        } catch (e: CancellationException) {
            throw e
        } catch (_: Exception) {
            // Nothing was pinned, so nothing is locked.
        }
    }

    /** A group: its name decrypted with the group key, and every member resolved. */
    private suspend fun hydrateGroup(
        dto: ConversationDto,
        identity: SelfIdentity,
        conversationKey: ByteArray,
        participants: List<ParticipantDto>,
    ): ConversationMeta {
        val name = if (dto.nameCiphertext != null && dto.nameNonce != null) {
            runCatching {
                crypto.decryptWithConversationKey(Base64Codec.decode(dto.nameCiphertext), Base64Codec.decode(dto.nameNonce), conversationKey)
            }.getOrNull()
        } else {
            null
        }
        val members = coroutineScope {
            participants.map { p ->
                async {
                    runCatching { apiCall(ApiClient.json) { api.getUser(p.userId) } }.getOrNull()?.let { user ->
                        GroupMemberView(p.userId, user.username, user.avatarUrl, p.role, user.publicKey, p.userId == identity.userId)
                    }
                }
            }.awaitAll().filterNotNull()
        }
        return ConversationMeta(
            id = dto.id,
            otherUserId = "",
            handle = name?.takeIf { it.isNotBlank() } ?: DEFAULT_GROUP_NAME,
            avatarUrl = null,
            otherPublicKey = ByteArray(0),
            otherPublicKeyB64 = "",
            conversationKey = conversationKey,
            safetyNumber = emptyList(),
            fetchedAtMs = System.currentTimeMillis(),
            isGroup = true,
            members = members,
            myRole = dto.role,
        )
    }

    /**
     * Port of hydrateCommunity in ui/src/state/actions.ts. A community conversation's keys are the member's
     * epoch keys (not one group key), its name is the community's public name, and its member list is only
     * who appears in the loaded messages ([previous]'s, kept across refreshes): a channel's subscribers
     * can't list its members. Cheap when nothing changed: the ring is re-fetched only when the conversation
     * list reports a new epoch or a pending rotation.
     */
    suspend fun hydrateCommunity(dto: ConversationDto, identity: SelfIdentity, previous: ConversationMeta?): ConversationMeta {
        val info = requireNotNull(dto.community)
        val keys = communities ?: throw IllegalStateException("Communities aren't available")
        val ring = keys.ensure(info.id, dto.id, info.keyEpoch, info.rekeyRequired)
        val currentKey = ring.keys[ring.currentEpoch] ?: ring.keys.entries.maxByOrNull { it.key }?.value
        val self = GroupMemberView(identity.userId, identity.handle, identity.avatarUrl, info.role, Base64Codec.encode(identity.publicKey), isSelf = true)
        return ConversationMeta(
            id = dto.id,
            otherUserId = "",
            handle = info.name.ifBlank { DEFAULT_GROUP_NAME },
            avatarUrl = null,
            otherPublicKey = ByteArray(0),
            otherPublicKeyB64 = "",
            // A copy: closing the session overwrites this, and the ring keeps its own.
            conversationKey = currentKey?.copyOf() ?: ByteArray(E2eCrypto.AEAD_KEY_BYTES),
            safetyNumber = emptyList(),
            fetchedAtMs = System.currentTimeMillis(),
            isGroup = true,
            members = listOf(self) + previous?.members?.filter { !it.isSelf }.orEmpty(),
            myRole = dto.role,
            community = CommunityLink(info.id, info.kind, info.role, info.canPost, info.status, info.keyEpoch, info.rekeyRequired),
        )
    }

    /** A member seen posting in a community, for showing their name. Null if they can't be looked up. */
    suspend fun communityPoster(userId: String, role: String = "member"): GroupMemberView? =
        runCatching { apiCall(ApiClient.json) { api.getUser(userId) } }.getOrNull()
            ?.let { GroupMemberView(userId, it.username, it.avatarUrl, role, it.publicKey, isSelf = false) }

    /**
     * Fetches and decrypts a DM's live messages. `unread` counts incoming
     * messages this user has no receipt for yet; own messages are marked
     * seen/delivered from the other side's receipts.
     */
    suspend fun fetchMessages(
        meta: ConversationMeta,
        selfUserId: String,
        /** How many of the newest messages to fetch: [MessagePaging.PAGE_SIZE], more once older ones were asked for. */
        window: Int = MessagePaging.PAGE_SIZE,
    ): MessagesResult = coroutineScope {
        val dtosCall = async {
            MessagePaging.fetchLatest(window) { limit, before -> apiCall(ApiClient.json) { api.listMessages(meta.id, limit, before) } }
        }
        val receiptsCall = async { apiCall(ApiClient.json) { api.listReceipts(meta.id) } }
        val latest = dtosCall.await()
        val dtos = latest.messages
        val receipts = receiptsCall.await()

        // A community's messages carry no sender signatures and open with the community's own epoch keys.
        if (meta.community == null) {
            keys?.forgetKeylessSenders(dtos.filter { it.deletedAt == null && it.signature != null }.map { it.senderId })
            keys?.resolveSenderKeys(selfUserId, dtos.filter { it.deletedAt == null }.map { it.senderId })
            // A message under a key epoch this session hasn't opened yet: someone just rotated.
            if (keys != null && dtos.any { it.deletedAt == null && keys.ringFor(meta.id)?.keys?.containsKey(it.keyEpoch) == false }) {
                runCatching { keys.refresh(meta.id) }
            }
        }
        withContext(Dispatchers.Default) {
            val live = withoutReplays(dtos.filter { it.deletedAt == null && it.ciphertext != null && it.nonce != null })
            // Deleted for everyone: a pin of one of these drops out of the pinned bar.
            val deletedIds = dtos.filter { it.deletedAt != null }.mapTo(HashSet()) { it.id.lowercase() }
            val mine = receipts.filter { it.userId == selfUserId }.mapTo(HashSet()) { it.messageId }
            // Read/delivered once EVERY other participant has read/received it
            // (for a DM, just the one other person; "read" implies delivered).
            val others = meta.otherUserIds(selfUserId)
            val readBy = HashMap<String, MutableSet<String>>()
            val deliveredBy = HashMap<String, MutableSet<String>>()
            for (r in receipts) {
                if (r.userId !in others) continue
                deliveredBy.getOrPut(r.messageId) { HashSet() } += r.userId
                if (r.status == "read") readBy.getOrPut(r.messageId) { HashSet() } += r.userId
            }
            val theirRead = readBy.filterValues { it.size >= others.size && others.isNotEmpty() }.keys
            val theirDelivered = deliveredBy.filterValues { it.size >= others.size && others.isNotEmpty() }.keys

            val hiddenIds = hidden?.load(selfUserId).orEmpty()
            // Communities: each message names the epoch whose key encrypted it.
            val communityRing = if (meta.community != null) communities?.ringFor(meta.id) else null
            // Reactions arrive as their own messages; fold them into the
            // message they react to and never show them on their own. Edits,
            // votes and a poll's end are folded the same way; pins stay as
            // notices. None of them count as unread.
            val quietIds = HashSet<String>()
            val reactions = HashMap<String, MutableMap<String, ReactionTally>>()
            val edits = ArrayList<EditEvent>()
            val pollEvents = ArrayList<PollEvent>()
            val senderOf = HashMap<String, String>()
            val shown = live.mapNotNull { dto ->
                when (val decoded = decode(dto, meta, communityRing, selfUserId, theirRead, theirDelivered)) {
                    is Decoded.Reaction -> {
                        quietIds += dto.id
                        val tally = reactions.getOrPut(decoded.targetId) { HashMap() }.getOrPut(decoded.emoji) { ReactionTally(decoded.atMs) }
                        tally.firstAtMs = minOf(tally.firstAtMs, decoded.atMs)
                        tally.senders += dto.senderId
                        if (dto.senderId == selfUserId) tally.myReactionId = dto.id
                        null
                    }
                    is Decoded.Edit -> {
                        quietIds += dto.id
                        edits += decoded.event
                        null
                    }
                    is Decoded.Poll -> {
                        quietIds += dto.id
                        pollEvents += decoded.event
                        null
                    }
                    Decoded.Hidden -> {
                        quietIds += dto.id
                        null
                    }
                    is Decoded.Shown -> {
                        val notice = decoded.message.notice
                        if (notice != null) quietIds += dto.id
                        val gone = notice?.kind == NoticeKind.Pin && (notice.targetId in deletedIds || hiddenIds.any { it.equals(notice.targetId, ignoreCase = true) })
                        val message = if (gone) decoded.message.copy(notice = notice.copy(targetGone = true)) else decoded.message
                        message.takeIf { it.id !in hiddenIds }?.also { senderOf[it.id.lowercase()] = dto.senderId }
                    }
                }
            }
            val messages = MessageFolding.foldAll(shown, edits, pollEvents, senderOf, selfUserId).map { message ->
                val byEmoji = reactions[message.id.lowercase()] ?: return@map message
                message.copy(reactions = MessageFolding.orderReactions(byEmoji))
            }.sortedBy { it.createdAtMs }
            val unread = live.count { it.senderId != selfUserId && it.id !in mine && it.id !in quietIds }
            MessagesResult(messages, unread, latest.hasOlder)
        }
    }

    suspend fun markRead(conversationId: String, readReceipts: Boolean) {
        apiCall(ApiClient.json) { api.markRead(conversationId, MarkReadRequest(if (readReceipts) "read" else "delivered")) }
    }

    /**
     * Stage 1: encrypt locally (the plaintext never reaches the network).
     * Stage 2: relay the ciphertext; only true once the server answers with a
     * persisted id. Stage 3: recompute the commitment and compare it with the
     * server's.
     */
    /** A plain message, or a reply carrying its quote inside the encrypted body. */
    suspend fun send(meta: ConversationMeta, text: String, replyTo: ReplyTarget? = null, clientMessageId: String? = null): Message {
        // Leading NULs are reserved for the structured-message markers.
        val trimmed = text.trimStart('\u0000').trim()
        require(trimmed.isNotEmpty()) { "Empty message" }
        val body = if (replyTo != null) RichMessage.serializeReply(trimmed, replyTo.ref) else trimmed
        return sendEnvelope(meta, body, attachments = null, clientMessageId = clientMessageId).copy(
            kind = MessageKind.Text,
            text = trimmed,
            replyTo = replyTo?.let { ReplyView(it.ref.messageId, RichMessage.makePreview(it.ref.preview), it.fromMe) },
        )
    }

    /** Reacts to a message by sending a small encrypted reaction message. */
    suspend fun sendReaction(meta: ConversationMeta, messageId: String, emoji: String) {
        sendEnvelope(meta, RichMessage.serializeReaction(messageId, emoji), attachments = null)
    }

    /** A new text for one of this user's own messages; everyone else ignores edits from anyone but its sender. */
    suspend fun sendEdit(meta: ConversationMeta, messageId: String, text: String) {
        sendEnvelope(meta, RichMessage.serializeEdit(cleanText(text), messageId), attachments = null)
    }

    /** A reply, or a quick reaction emoji, to one of a friend's stories: a chat message to its author that says which story. */
    suspend fun sendStoryReply(meta: ConversationMeta, text: String, ref: StoryReplyRef): Message {
        val clean = cleanText(text)
        return sendEnvelope(meta, RichMessage.serializeStoryReply(clean, ref), attachments = null).copy(kind = MessageKind.Text, text = clean, storyReply = ref)
    }

    /** Text from another chat, marked as forwarded. */
    suspend fun sendForwarded(meta: ConversationMeta, text: String): Message {
        val clean = cleanText(text)
        return sendEnvelope(meta, RichMessage.serializeForwarded(clean), attachments = null).copy(text = clean, forwarded = true)
    }

    /** A channel post forwarded into a chat, end-to-end encrypted like any message; it keeps which post it was. */
    suspend fun sendChannelForward(meta: ConversationMeta, text: String, ref: ChannelPostRef): Message {
        val clean = cleanText(text)
        return sendEnvelope(meta, RichMessage.serializeChannelForward(clean, ref), attachments = null)
            .copy(text = clean, forwarded = true, channelPost = ref)
    }

    suspend fun sendPin(meta: ConversationMeta, messageId: String, preview: String) {
        sendEnvelope(meta, RichMessage.serializePin(messageId, preview), attachments = null)
    }

    suspend fun sendUnpin(meta: ConversationMeta, messageId: String) {
        sendEnvelope(meta, RichMessage.serializeUnpin(messageId), attachments = null)
    }

    /** Throws [IllegalArgumentException] unless there's a question and 2-10 different options. */
    suspend fun sendPoll(meta: ConversationMeta, question: String, options: List<String>, multiple: Boolean) {
        sendEnvelope(meta, RichMessage.serializePoll(question, options, multiple), attachments = null)
    }

    /** [labels]: the poll's options, for the text older apps show instead. */
    suspend fun sendVote(meta: ConversationMeta, pollId: String, options: List<Int>, labels: List<String>) {
        sendEnvelope(meta, RichMessage.serializeVote(pollId, options, labels), attachments = null)
    }

    suspend fun sendEndPoll(meta: ConversationMeta, pollId: String) {
        sendEnvelope(meta, RichMessage.serializeEndPoll(pollId), attachments = null)
    }

    // Leading NULs are reserved for the structured-message markers.
    private fun cleanText(text: String): String = text.trimStart('\u0000').trim().also { require(it.isNotEmpty()) { "Empty message" } }

    /**
     * Sends a file's descriptor as an ordinary E2EE message listing its
     * object as the attachment. Safe to retry: re-sending the same
     * attachment returns the original message instead of a duplicate.
     */
    suspend fun sendFile(meta: ConversationMeta, descriptor: FileDescriptor, clientMessageId: String? = null): Message =
        sendEnvelope(meta, FileMessage.serialize(descriptor), attachments = listOf(descriptor.objectId), clientMessageId = clientMessageId)
            .copy(kind = MessageKind.File, text = descriptor.filename, file = descriptor, fileStatus = "available")

    /** A community's messages use its own epoch keys ([sendToCommunity]); every other chat signs under its group key ([sendToConversation]). */
    private suspend fun sendEnvelope(meta: ConversationMeta, plaintext: String, attachments: List<String>?, clientMessageId: String? = null): Message {
        if (meta.community != null) return sendToCommunity(meta, meta.community, plaintext, attachments, clientMessageId)
        return sendToConversation(meta, plaintext, attachments, clientMessageId)
    }

    /**
     * Encrypts with the conversation's current key (epoch) and signs. When the
     * server says the group needs a new key (someone left) or the key changed,
     * rotates or re-reads the keys and sends again.
     */
    private suspend fun sendToConversation(meta: ConversationMeta, plaintext: String, attachments: List<String>?, clientMessageId: String?): Message =
        withContext(Dispatchers.Default) {
            var attempt = 0
            while (true) {
                val (key, epoch) = keys?.currentKey(meta.id, meta.conversationKey) ?: (meta.conversationKey to 0)
                val encrypted = crypto.encryptWithConversationKey(plaintext, key)
                val signature = keys?.signatureFor(meta.id, epoch, encrypted.nonce, encrypted.ciphertext)
                val request = SendMessageRequest(
                    Base64Codec.encode(encrypted.ciphertext), Base64Codec.encode(encrypted.nonce), attachments,
                    keyEpoch = if (keys != null) epoch else null, signature = signature, clientMessageId = clientMessageId,
                )
                try {
                    val dto = apiCall(ApiClient.json) { api.sendMessage(meta.id, request) }
                    val storedCiphertext = dto.ciphertext?.let(Base64Codec::decode)
                    val storedNonce = dto.nonce?.let(Base64Codec::decode)
                    if (clientMessageId != null && storedCiphertext != null && storedNonce != null &&
                        !storedCiphertext.contentEquals(encrypted.ciphertext)
                    ) {
                        // A retry of a send whose answer was lost: the server returned the copy it stored
                        // the first time. Only this account knows its clientMessageId; check it's ours anyway.
                        val same = runCatching { crypto.decryptWithConversationKey(storedCiphertext, storedNonce, key) == plaintext }.getOrNull()
                        if (same == false) throw ApiException(409, "message_mismatch", "The server returned a different message.")
                        return@withContext sentMessage(dto, storedCiphertext, storedNonce)
                    }
                    return@withContext sentMessage(dto, encrypted.ciphertext, encrypted.nonce)
                } catch (e: ApiException) {
                    if (keys == null || ++attempt >= MAX_KEY_REFRESHES) throw e
                    when (e.code) {
                        "group_rekey_required" -> keys.rotate(meta.id)
                        "stale_key_epoch" -> keys.refresh(meta.id)
                        else -> throw e
                    }
                }
            }
            @Suppress("UNREACHABLE_CODE")
            error("unreachable")
        }

    private fun sentMessage(dto: MessageDto, ciphertext: ByteArray, nonce: ByteArray): Message {
            val ciphertextB64 = Base64Codec.encode(ciphertext)
            val nonceB64 = Base64Codec.encode(nonce)
            return Message(
                id = dto.id,
                fromMe = true,
                kind = MessageKind.Text,
                text = "",
                createdAtMs = parseMs(dto.createdAt),
                deleteAtMs = dto.deleteAt?.let(::parseMs),
                security = buildSecurity(dto.id, ciphertext, nonce, dto.verification),
                ciphertextB64 = ciphertextB64,
                nonceB64 = nonceB64,
            )
    }

    /**
     * Port of sendToCommunity in ui/src/lib/e2eV2/messaging.ts: always the group key of the current epoch.
     * If someone left since this phone loaded the keys, the server answers rekey_required: this app rotates
     * the key (sealing a new one only to who remains) and sends under it; stale_key_epoch means another app
     * already did, so it reloads the keys and tries again. Two retries at most.
     */
    private suspend fun sendToCommunity(
        meta: ConversationMeta,
        community: CommunityLink,
        plaintext: String,
        attachments: List<String>?,
        clientMessageId: String?,
    ): Message {
        val keys = communities ?: throw IllegalStateException("Communities aren't available")
        var attempt = 0
        while (true) {
            val ring = keys.ringFor(meta.id) ?: keys.load(community.id, meta.id)
            val encrypted = withContext(Dispatchers.Default) {
                try {
                    keys.encrypt(ring, plaintext)
                } catch (_: EpochKeyException) {
                    throw UserFacingException("You don't have this community's key yet. It arrives once a member's app is online.")
                }
            }
            val ciphertextB64 = Base64Codec.encode(encrypted.ciphertext)
            val nonceB64 = Base64Codec.encode(encrypted.nonce)
            try {
                val dto = apiCall(ApiClient.json) {
                    api.sendMessage(
                        meta.id,
                        SendMessageRequest(ciphertextB64, nonceB64, attachments, keyEpoch = encrypted.keyEpoch.toInt(), clientMessageId = clientMessageId),
                    )
                }
                return sentMessage(dto, encrypted.ciphertext, encrypted.nonce)
            } catch (e: ApiException) {
                if (attempt++ >= 2) throw e
                when (e.code) {
                    "rekey_required" -> try {
                        keys.rotate(community.id, meta.id)
                    } catch (rotateError: ApiException) {
                        if (rotateError.status == 401) throw rotateError
                        keys.load(community.id, meta.id)
                    }
                    "stale_key_epoch" -> keys.load(community.id, meta.id)
                    else -> throw e
                }
            }
        }
    }


    /** Re-verifies an own message loaded from a poll: fetches the server's record fresh and recomputes locally. */
    suspend fun verify(message: Message): MessageSecurity {
        val ciphertext = message.ciphertextB64 ?: return MessageSecurity(VerificationStatus.Unavailable)
        val nonce = message.nonceB64 ?: return MessageSecurity(VerificationStatus.Unavailable)
        return try {
            val record = apiCall(ApiClient.json) { api.getMessageVerification(message.id) }
            buildSecurity(message.id, Base64Codec.decode(ciphertext), Base64Codec.decode(nonce), record)
        } catch (_: Exception) {
            MessageSecurity(VerificationStatus.Unavailable)
        }
    }

    suspend fun delete(messageId: String) {
        apiCall(ApiClient.json) { api.deleteMessage(messageId) }
    }

    suspend fun setDisappearing(conversationId: String, duration: Disappearing) {
        val body = buildJsonObject { put("seconds", duration.seconds?.let(::JsonPrimitive) ?: JsonNull) }
        apiCall(ApiClient.json) { api.setDisappearing(conversationId, body) }
    }

    private sealed interface Decoded {
        class Shown(val message: Message) : Decoded
        class Reaction(val targetId: String, val emoji: String, val atMs: Long) : Decoded
        class Edit(val event: EditEvent) : Decoded
        class Poll(val event: PollEvent) : Decoded
        /** An unverified message trying to act on another one: dropped. */
        object Hidden : Decoded
    }

    private fun decode(
        dto: MessageDto,
        meta: ConversationMeta,
        /** A community conversation's epoch keys (null for every other chat). */
        communityRing: CommunityRing?,
        selfUserId: String,
        theirRead: Set<String>,
        theirDelivered: Set<String>,
    ): Decoded {
        val fromMe = dto.senderId == selfUserId
        // The key of the epoch it was sent under, and the sender's signature. A community's messages aren't signed.
        val ring = if (meta.community == null) keys?.ringFor(meta.id) else null
        val check = if (meta.community == null) keys?.checkSender(dto, meta.id) ?: SenderCheck.Legacy else SenderCheck.Legacy
        val plaintext = if (check == SenderCheck.Invalid) {
            null
        } else {
            try {
                val ciphertext = Base64Codec.decode(dto.ciphertext!!)
                val nonce = Base64Codec.decode(dto.nonce!!)
                if (meta.community != null) {
                    // A community message opens only with its own epoch's key (null: from before this member joined).
                    if (communityRing != null) communities?.decrypt(communityRing, dto.keyEpoch.toLong(), ciphertext, nonce)
                    else crypto.decryptWithConversationKey(ciphertext, nonce, meta.conversationKey)
                } else {
                    val key = if (ring != null) ring.keys[dto.keyEpoch] ?: throw IllegalStateException("no key for this epoch") else meta.conversationKey
                    crypto.decryptWithConversationKey(ciphertext, nonce, key)
                }
            } catch (_: Exception) {
                null
            }
        }
        val unverified = check == SenderCheck.Unverified
        // A file only counts if the server also lists that exact object as this
        // message's attachment, so a descriptor can't point at some other
        // object and a pasted one doesn't render as a file.
        val descriptor = plaintext?.let(FileMessage::parse)
        val attachment = descriptor?.let { d -> dto.attachments.firstOrNull { it.objectId.lowercase() == d.objectId } }
        val structured = plaintext?.takeIf(RichMessage::isStructured)?.let(RichMessage::parse)
        val createdAtMs = parseMs(dto.createdAt)
        val acts = structured is ParsedBody.Reaction ||
            (structured as? ParsedBody.Text)?.let { it.edits != null || it.vote != null || it.endPoll != null || it.pin != null || it.unpin != null } == true
        // Edits, reactions, votes and pins only count when we know who sent them.
        if (unverified && acts) return Decoded.Hidden
        if (structured is ParsedBody.Reaction) return Decoded.Reaction(structured.messageId, structured.emoji, createdAtMs)
        val rich = structured as? ParsedBody.Text
        if (rich?.edits != null) return Decoded.Edit(EditEvent(dto.id, rich.edits, dto.senderId, rich.text, createdAtMs))
        if (rich?.vote != null || rich?.endPoll != null) {
            return Decoded.Poll(
                PollEvent(rich.vote?.pollId ?: rich.endPoll!!, dto.senderId, createdAtMs, end = rich.vote == null, options = rich.vote?.options.orEmpty()),
            )
        }
        val (kind, text) = when {
            check == SenderCheck.Invalid -> MessageKind.Undecryptable to "Couldn't verify who sent this message."
            plaintext == null -> MessageKind.Undecryptable to "Couldn't decrypt this message."
            attachment != null -> MessageKind.File to descriptor.filename
            // A poll shows its question; a pin notice the pinned message's preview.
            rich?.poll != null -> MessageKind.Text to rich.poll.question
            rich?.pin != null -> MessageKind.Text to rich.pin.preview
            rich?.unpin != null -> MessageKind.Text to ""
            rich != null -> MessageKind.Text to rich.text
            structured == ParsedBody.Invalid || FileMessage.isFileMessage(plaintext) -> MessageKind.Text to "Unreadable message."
            else -> MessageKind.Text to plaintext
        }
        val reply = rich?.replyTo
        val shownRich = rich.takeIf { attachment == null && plaintext != null }
        return Decoded.Shown(Message(
            id = dto.id,
            fromMe = fromMe,
            senderId = dto.senderId,
            kind = kind,
            text = text,
            forwarded = shownRich?.forwarded == true,
            channelPost = shownRich?.channelPost,
            storyReply = shownRich?.storyReply,
            poll = shownRich?.poll?.let { PollView(it.question, it.options, it.multiple) },
            notice = when {
                shownRich?.pin != null -> NoticeView(NoticeKind.Pin, shownRich.pin.messageId)
                shownRich?.unpin != null -> NoticeView(NoticeKind.Unpin, shownRich.unpin)
                else -> null
            },
            file = if (attachment != null) descriptor else null,
            fileStatus = attachment?.status,
            createdAtMs = createdAtMs,
            deleteAtMs = dto.deleteAt?.let(::parseMs),
            seen = fromMe && dto.id in theirRead,
            delivered = fromMe && (dto.id in theirDelivered || dto.id in theirRead),
            ciphertextB64 = if (fromMe) dto.ciphertext else null,
            nonceB64 = if (fromMe) dto.nonce else null,
            replyTo = reply?.let { ReplyView(it.messageId, it.preview, it.senderId == selfUserId.lowercase()) },
            unverifiedSender = unverified,
        ))
    }

    private companion object {
        const val DEFAULT_GROUP_NAME = "Group"
        const val MAX_KEY_REFRESHES = 3

        fun parseMs(iso: String): Long = Instant.parse(iso).toEpochMilli()

        fun buildSecurity(messageId: String, ciphertext: ByteArray, nonce: ByteArray, record: VerificationDto?): MessageSecurity {
            if (record == null || record.protocolVersion != Commitment.PROTOCOL_VERSION) {
                return MessageSecurity(VerificationStatus.Unavailable)
            }
            val local = Commitment.computeHex(messageId, ciphertext, nonce)
            return MessageSecurity(
                verification = if (Commitment.matches(local, record.commitment)) VerificationStatus.Verified else VerificationStatus.Failed,
                publicTransactionId = record.publicTransactionId,
                protocolVersion = record.protocolVersion,
                localCommitment = local,
                networkCommitment = record.commitment,
            )
        }
    }
}
