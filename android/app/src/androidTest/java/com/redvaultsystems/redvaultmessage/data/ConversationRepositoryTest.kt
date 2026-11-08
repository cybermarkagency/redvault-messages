// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.goterl.lazysodium.LazySodiumAndroid
import com.goterl.lazysodium.SodiumAndroid
import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.Commitment
import com.redvaultsystems.redvaultmessage.crypto.E2eCrypto
import com.redvaultsystems.redvaultmessage.data.api.*
import com.redvaultsystems.redvaultmessage.data.api.AttachmentDto
import com.redvaultsystems.redvaultmessage.data.api.AuthResult
import com.redvaultsystems.redvaultmessage.data.api.ConversationDto
import com.redvaultsystems.redvaultmessage.data.api.LoginRequest
import com.redvaultsystems.redvaultmessage.data.api.LoginResponse
import com.redvaultsystems.redvaultmessage.data.api.MarkReadRequest
import com.redvaultsystems.redvaultmessage.data.api.MeResult
import com.redvaultsystems.redvaultmessage.data.api.MessageDto
import com.redvaultsystems.redvaultmessage.data.api.MfaCodeRequest
import com.redvaultsystems.redvaultmessage.data.api.ParticipantDto
import com.redvaultsystems.redvaultmessage.data.api.ReceiptDto
import com.redvaultsystems.redvaultmessage.data.api.RegisterRequest
import com.redvaultsystems.redvaultmessage.data.api.SendMessageRequest
import com.redvaultsystems.redvaultmessage.data.api.UserResult
import com.redvaultsystems.redvaultmessage.data.api.VerificationDto
import com.redvaultsystems.redvaultmessage.data.model.FileDescriptor
import com.redvaultsystems.redvaultmessage.data.model.FileEncryptionInfo
import com.redvaultsystems.redvaultmessage.data.model.FileMessage
import com.redvaultsystems.redvaultmessage.data.model.MessageKind
import com.redvaultsystems.redvaultmessage.data.model.ReplyRef
import com.redvaultsystems.redvaultmessage.data.model.RichMessage
import com.redvaultsystems.redvaultmessage.data.model.VerificationStatus
import com.redvaultsystems.redvaultmessage.identity.SelfIdentity
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

private const val FILE_OBJECT_ID = "3f2504e0-4f89-11d3-9a0c-0305e82c3301"

@RunWith(AndroidJUnit4::class)
class ConversationRepositoryTest {
    private val crypto = E2eCrypto(LazySodiumAndroid(SodiumAndroid()))
    private val me = crypto.generateKeyPair()
    private val them = crypto.generateKeyPair()
    private val conversationKey = crypto.generateConversationKey()
    private val identity = SelfIdentity("me", "u-me", me.publicKey, me.privateKey, null, "user", "free", false)

    private fun envelope(text: String) = crypto.encryptWithConversationKey(text, conversationKey)

    private fun dto(id: String, sender: String, text: String, attachments: List<AttachmentDto> = emptyList()): MessageDto {
        val e = envelope(text)
        return MessageDto(
            id = id, conversationId = "c1", senderId = sender,
            ciphertext = Base64Codec.encode(e.ciphertext), nonce = Base64Codec.encode(e.nonce),
            createdAt = "2026-09-24T10:00:0${id.last()}Z", deleteAt = "2027-03-24T10:00:00Z",
            attachments = attachments,
        )
    }

    private fun fileDescriptor(filename: String) = FileMessage.serialize(
        FileDescriptor(
            type = "file", version = 1, objectId = FILE_OBJECT_ID, fileKey = "AAAA", filename = filename,
            mimeType = "text/plain", originalSize = 1, encryptedSize = 18, ciphertextSha256 = "0".repeat(64),
            encryption = FileEncryptionInfo("xchacha20poly1305-secretstream", 1, "AAAA", 65536),
        ),
    )

    private val fake = FakeApi(
        conversation = ConversationDto(
            id = "c1", isGroup = false, role = "member",
            wrappedKey = Base64Codec.encode(crypto.sealConversationKey(conversationKey, me.publicKey)),
            disappearingSeconds = 3600, createdBy = "u-them", createdAt = "2026-09-24T09:00:00Z",
        ),
        them = UserResult("u-them", "them", Base64Codec.encode(them.publicKey), null, "2026-01-01T00:00:00Z"),
    )
    private val repository = ConversationRepository(fake, crypto)

    @Test
    fun hydratesDecryptsAndComputesReadState() = runBlocking {
        fake.messages = listOf(
            dto("m1", "u-them", "hi from them"),
            dto("m2", "u-me", "hi back"),
            dto("m3", "u-me", "still there?"),
            dto("m4", "u-them", "yes"),
            dto("m5", "u-them", fileDescriptor("notes\u202E.txt"), listOf(AttachmentDto(FILE_OBJECT_ID, "available"))),
            dto("m6", "u-them", "\u0000rv-file:{\"filename\":\"forged.txt\"}"),
        )
        fake.receipts = listOf(
            ReceiptDto("m1", "u-me", "delivered"),
            ReceiptDto("m2", "u-them", "read"),
            ReceiptDto("m3", "u-them", "delivered"),
        )

        val meta = repository.hydrateMeta(repository.listConversations().single(), identity)!!
        assertEquals("them", meta.handle)
        assertEquals(crypto.safetyNumberDigits(me.publicKey, them.publicKey), meta.safetyNumber)

        val result = repository.fetchMessages(meta, identity.userId)
        val byId = result.messages.associateBy { it.id }
        assertEquals(listOf("m1", "m2", "m3", "m4", "m5", "m6"), result.messages.map { it.id })
        assertEquals("hi from them", byId.getValue("m1").text)
        assertTrue(byId.getValue("m2").seen && byId.getValue("m2").delivered)
        assertTrue(!byId.getValue("m3").seen && byId.getValue("m3").delivered)
        // m4, m5, m6 are incoming with no receipt from me.
        assertEquals(3, result.unread)
        // A real file message: bidi control characters stripped from the name.
        assertEquals(MessageKind.File, byId.getValue("m5").kind)
        assertEquals("notes.txt", byId.getValue("m5").text)
        // A descriptor with no server-side attachment is not treated as a file.
        assertEquals(MessageKind.Text, byId.getValue("m6").kind)
        assertEquals("Unreadable message.", byId.getValue("m6").text)
    }

    @Test
    fun foldsRepliesAndReactionsIntoTheirMessages() = runBlocking {
        val target = "11111111-1111-4111-8111-111111111111"
        val themId = "22222222-2222-4222-8222-222222222222"
        fun uuidDto(id: String, sender: String, body: String, second: Int) = dto(id, sender, body).copy(createdAt = "2026-09-24T10:00:${"%02d".format(second)}Z")
        fake.messages = listOf(
            uuidDto(target, "u-them", "want lunch?", 1),
            uuidDto("r1", "u-me", RichMessage.serializeReaction(target, "👍"), 2),
            uuidDto("r2", "u-them", RichMessage.serializeReaction(target, "👍"), 3),
            uuidDto("r3", "u-them", RichMessage.serializeReaction(target, "❤️"), 4),
            uuidDto("m9", "u-me", RichMessage.serializeReply("yes!", ReplyRef(target, themId, "want lunch?")), 5),
        )
        fake.receipts = emptyList()
        val meta = repository.hydrateMeta(repository.listConversations().single(), identity)!!
        val result = repository.fetchMessages(meta, identity.userId)

        // Reactions never show as messages of their own.
        assertEquals(listOf(target, "m9"), result.messages.map { it.id })
        val reactions = result.messages.first().reactions
        assertEquals(listOf("👍", "❤️"), reactions.map { it.emoji })
        assertEquals(2, reactions[0].count)
        assertTrue(reactions[0].mine)
        assertEquals("r1", reactions[0].myReactionId)
        assertFalse(reactions[1].mine)
        // The reply decodes to its text plus the quote.
        val reply = result.messages.last()
        assertEquals("yes!", reply.text)
        assertEquals(target, reply.replyTo?.messageId)
        assertEquals("want lunch?", reply.replyTo?.preview)
        // Their two reactions don't count as unread; only their one message does.
        assertEquals(1, result.unread)
    }

    @Test
    fun hydratesAGroupWithItsEncryptedNameAndMembers() = runBlocking {
        val name = crypto.encryptWithConversationKey("Weekend crew", conversationKey)
        val groupApi = FakeApi(
            conversation = ConversationDto(
                id = "g1", isGroup = true, role = "admin",
                nameCiphertext = Base64Codec.encode(name.ciphertext), nameNonce = Base64Codec.encode(name.nonce),
                wrappedKey = Base64Codec.encode(crypto.sealConversationKey(conversationKey, me.publicKey)),
                disappearingSeconds = null, createdBy = "u-them", createdAt = "2026-09-24T09:00:00Z",
            ),
            them = UserResult("u-them", "them", Base64Codec.encode(them.publicKey), null, "2026-01-01T00:00:00Z"),
        )
        groupApi.messages = listOf(dto("m1", "u-them", "hi group"))
        val groups = ConversationRepository(groupApi, crypto)
        val meta = groups.hydrateMeta(groups.listConversations().single(), identity)!!

        assertTrue(meta.isGroup)
        assertEquals("Weekend crew", meta.handle)
        assertEquals("admin", meta.myRole)
        assertEquals(listOf("u-them"), meta.otherUserIds(identity.userId))
        assertEquals("them", meta.handleOf("u-them"))
        val message = groups.fetchMessages(meta, identity.userId).messages.single()
        assertEquals("hi group", message.text)
        assertEquals("u-them", message.senderId)
    }

    @Test
    fun sendEncryptsAndVerifiesTheCommitment() = runBlocking {
        val meta = repository.hydrateMeta(repository.listConversations().single(), identity)!!
        val sent = repository.send(meta, "  secret plan  ")

        assertEquals("secret plan", sent.text)
        assertEquals(VerificationStatus.Verified, sent.security?.verification)
        // The server only ever saw ciphertext, which decrypts to the message.
        val request = fake.lastSent!!
        assertFalse(request.ciphertext.contains("secret"))
        assertEquals(
            "secret plan",
            crypto.decryptWithConversationKey(Base64Codec.decode(request.ciphertext), Base64Codec.decode(request.nonce), conversationKey),
        )
    }

    @Test
    fun mismatchedServerCommitmentIsAFailureNotASuccess() = runBlocking {
        fake.tamperCommitment = true
        val meta = repository.hydrateMeta(repository.listConversations().single(), identity)!!
        assertEquals(VerificationStatus.Failed, repository.send(meta, "hello").security?.verification)
    }
}

private class FakeApi(private val conversation: ConversationDto, private val them: UserResult) : ApiService {
    var messages: List<MessageDto> = emptyList()
    var receipts: List<ReceiptDto> = emptyList()
    var lastSent: SendMessageRequest? = null
    var tamperCommitment = false

    override suspend fun listConversations(include: String?) = listOf(conversation)
    override suspend fun listParticipants(conversationId: String) =
        listOf(ParticipantDto("u-me", "owner", "2026-01-01T00:00:00Z"), ParticipantDto(them.userId, "member", "2026-01-01T00:00:00Z"))
    override suspend fun getUser(userId: String) = them
    override suspend fun listMessages(conversationId: String, limit: Int?, before: String?) = messages
    override suspend fun listReceipts(conversationId: String) = receipts

    override suspend fun sendMessage(conversationId: String, body: SendMessageRequest): MessageDto {
        lastSent = body
        val id = "123e4567-e89b-12d3-a456-426614174000"
        val commitment = Commitment.computeHex(id, Base64Codec.decode(body.ciphertext), Base64Codec.decode(body.nonce))
        return MessageDto(
            id = id, conversationId = conversationId, senderId = "u-me",
            ciphertext = body.ciphertext, nonce = body.nonce, createdAt = "2026-09-24T10:00:00Z",
            verification = VerificationDto(
                publicTransactionId = "tx-1",
                commitment = if (tamperCommitment) commitment.reversed() else commitment,
                protocolVersion = 1, relayStatus = "relayed", createdAt = "2026-09-24T10:00:00Z",
            ),
        )
    }

    override suspend fun register(body: RegisterRequest): AuthResult = error("unused")
    override suspend fun login(body: LoginRequest): LoginResponse = error("unused")
    override suspend fun verifyMfa(body: MfaCodeRequest): LoginResponse = error("unused")
    override suspend fun logout() = error("unused")
    override suspend fun me(): MeResult = error("unused")
    override suspend fun listConversationRequests(): List<ConversationDto> = error("unused")
    override suspend fun acceptConversationRequest(conversationId: String) = error("unused")
    override suspend fun ignoreConversationRequest(conversationId: String) = error("unused")
    override suspend fun markRead(conversationId: String, body: MarkReadRequest) = Unit
    override suspend fun setDisappearing(conversationId: String, body: JsonObject) = Unit
    override suspend fun deleteMessage(messageId: String) = Unit
    override suspend fun getMessageVerification(messageId: String): VerificationDto = error("unused")
    override suspend fun lookupUser(username: String): UserLookupResult = error("unused")
    override suspend fun sendFriendRequest(body: UsernameRequest): FriendRequestDto = error("unused")
    override suspend fun incomingFriendRequests(): List<FriendRequestDto> = error("unused")
    override suspend fun outgoingFriendRequests(): List<FriendRequestDto> = error("unused")
    override suspend fun acceptFriendRequest(requestId: String, body: AcceptFriendRequest): CreatedConversation = error("unused")
    override suspend fun denyFriendRequest(requestId: String) = error("unused")
    override suspend fun cancelFriendRequest(requestId: String) = error("unused")
    override suspend fun updateSettings(body: SettingsRequest) = error("unused")
    override suspend fun changePassword(body: ChangePasswordRequest) = error("unused")
    override suspend fun changeUsername(body: UsernameRequest) = error("unused")
    override suspend fun setupMfa(): MfaSetupResult = error("unused")
    override suspend fun confirmMfa(body: MfaCodeRequest) = error("unused")
    override suspend fun disableMfa(body: PasswordRequest) = error("unused")
    override suspend fun deleteAccount(body: PasswordRequest) = error("unused")
    override suspend fun uploadAvatar(body: AvatarUploadRequest): AvatarResult = error("unused")
    override suspend fun removeAvatar() = error("unused")
    override suspend fun submitReport(body: ReportRequest) = error("unused")
    override suspend fun transparencyFeed(limit: Int): List<TransparencyEventDto> = error("unused")
    override suspend fun fileConfig(): FileConfigDto = error("unused")
    override suspend fun fileStorageUsage(): FileStorageUsageDto = error("unused")
    override suspend fun planStatus(): PlanStatusDto = error("unused")
    override suspend fun cancelPlan(): PlanStatusDto = error("unused")
    override suspend fun resumePlan(): PlanStatusDto = error("unused")
    override suspend fun requestFileUpload(body: FileUploadRequest): FileUploadAuthorization = error("unused")
    override suspend fun completeFileUpload(objectId: String, body: JsonObject): FileStatusDto = error("unused")
    override suspend fun fileDownload(objectId: String): FileDownloadDto = error("unused")
    override suspend fun preLogin(body: PreLoginRequest): PreLoginResult = error("unused")
    override suspend fun releaseFile(objectId: String, body: JsonObject): FileStatusDto = error("unused")
    override suspend fun deleteFile(objectId: String) = Unit
    override suspend fun listAnnouncements(): List<AnnouncementDto> = error("unused")
    override suspend fun getAnnouncement(id: String): AnnouncementDetailDto = error("unused")
    override suspend fun markAnnouncementRead(id: String) = error("unused")
    override suspend fun voteAnnouncement(id: String, body: AnnouncementVoteRequest) = error("unused")
    override suspend fun replyToAnnouncement(id: String, body: AnnouncementReplyRequest): AnnouncementReplyDto = error("unused")
    override suspend fun listBlocked(): List<BlockedUserDto> = error("unused")
    override suspend fun block(body: BlockRequest) = error("unused")
    override suspend fun unblock(userId: String) = error("unused")
    override suspend fun setRecoveryKey(body: SetRecoveryRequest) = error("unused")
    override suspend fun recoveryStart(body: RecoveryStartRequest): RecoveryStartResult = error("unused")
    override suspend fun recoveryComplete(body: RecoveryCompleteRequest): LoginResponse = error("unused")
    override suspend fun createConversation(body: CreateConversationRequest): CreatedConversation = error("unused")
    override suspend fun addParticipant(conversationId: String, body: AddParticipantRequest) = error("unused")
    override suspend fun rotateGroupKey(conversationId: String, body: RotateKeyRequest): RotateKeyResult = error("unused")
    override suspend fun publishKeyNotice(conversationId: String, notice: KeyNoticeDto) = error("unused")
    override suspend fun registerSigningKey(body: SigningKeyRequest) = error("unused")
    override suspend fun removeParticipant(conversationId: String, userId: String) = error("unused")
    override suspend fun setParticipantRole(conversationId: String, userId: String, body: RoleRequest) = error("unused")
    override suspend fun renameConversation(conversationId: String, body: RenameRequest) = error("unused")
    override suspend fun listFriends(): List<FriendDto> = error("unused")
}
