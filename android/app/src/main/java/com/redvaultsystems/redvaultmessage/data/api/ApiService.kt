package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.json.JsonObject
import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.HTTP
import retrofit2.http.PATCH
import retrofit2.http.POST
import retrofit2.http.PUT
import retrofit2.http.Path
import retrofit2.http.Query

/** Retrofit view of the backend. Paths are relative to BuildConfig.API_BASE (which ends in "/api/"). */
interface ApiService {
    @POST("auth/register")
    suspend fun register(@Body body: RegisterRequest): AuthResult

    @POST("auth/prelogin")
    suspend fun preLogin(@Body body: PreLoginRequest): PreLoginResult

    @POST("auth/login")
    suspend fun login(@Body body: LoginRequest): LoginResponse

    /** Completes a login that answered mfaRequired. Uses the half-session cookie login() set. */
    @POST("auth/mfa/verify")
    suspend fun verifyMfa(@Body body: MfaCodeRequest): LoginResponse

    @POST("auth/logout")
    suspend fun logout()

    @GET("auth/me")
    suspend fun me(): MeResult

    @GET("users/{id}")
    suspend fun getUser(@Path("id") userId: String): UserResult

    /** `include=communities` adds community conversations; the server hides them from apps that don't ask. */
    @GET("conversations")
    suspend fun listConversations(@Query("include") include: String? = null): List<ConversationDto>

    /** Message requests: 1:1 chats a non-friend started with you, not accepted or ignored yet. */
    @GET("conversations/requests")
    suspend fun listConversationRequests(): List<ConversationDto>

    @POST("conversations/{id}/accept")
    suspend fun acceptConversationRequest(@Path("id") conversationId: String)

    @POST("conversations/{id}/ignore")
    suspend fun ignoreConversationRequest(@Path("id") conversationId: String)

    @GET("conversations/{id}/participants")
    suspend fun listParticipants(@Path("id") conversationId: String): List<ParticipantDto>

    /**
     * The most recent [limit] (1-200, the server's default 50), or those created before [before] (an ISO
     * time) for older pages. See data/MessagePaging.kt.
     */
    @GET("conversations/{id}/messages")
    suspend fun listMessages(
        @Path("id") conversationId: String,
        @Query("limit") limit: Int? = null,
        @Query("before") before: String? = null,
    ): List<MessageDto>

    @POST("conversations/{id}/messages")
    suspend fun sendMessage(@Path("id") conversationId: String, @Body body: SendMessageRequest): MessageDto

    @GET("conversations/{id}/receipts")
    suspend fun listReceipts(@Path("id") conversationId: String): List<ReceiptDto>

    @POST("conversations/{id}/read")
    suspend fun markRead(@Path("id") conversationId: String, @Body body: MarkReadRequest)

    /** Body is `{"seconds": <int or null>}`; built as a JsonObject so an explicit null survives serialization. */
    @PATCH("conversations/{id}/disappearing")
    suspend fun setDisappearing(@Path("id") conversationId: String, @Body body: JsonObject)

    /** Deletes your own message for real: the server clears its ciphertext. */
    @DELETE("messages/{id}")
    suspend fun deleteMessage(@Path("id") messageId: String)

    @GET("messages/{id}/verification")
    suspend fun getMessageVerification(@Path("id") messageId: String): VerificationDto

    // --- Friends ---

    @GET("users/lookup")
    suspend fun lookupUser(@Query("username") username: String): UserLookupResult

    @POST("friend-requests")
    suspend fun sendFriendRequest(@Body body: UsernameRequest): FriendRequestDto

    @GET("friend-requests/incoming")
    suspend fun incomingFriendRequests(): List<FriendRequestDto>

    @GET("friend-requests/outgoing")
    suspend fun outgoingFriendRequests(): List<FriendRequestDto>

    /** The caller seals a fresh conversation key for both sides; the server creates the friendship and chat together. */
    @POST("friend-requests/{id}/accept")
    suspend fun acceptFriendRequest(@Path("id") requestId: String, @Body body: AcceptFriendRequest): CreatedConversation

    @POST("friend-requests/{id}/deny")
    suspend fun denyFriendRequest(@Path("id") requestId: String)

    @POST("friend-requests/{id}/cancel")
    suspend fun cancelFriendRequest(@Path("id") requestId: String)

    // --- Account ---

    @PATCH("auth/settings")
    suspend fun updateSettings(@Body body: SettingsRequest)

    /** Sends the new password with the private key re-wrapped under it, atomically. Revokes other sessions. */
    @PATCH("auth/password")
    suspend fun changePassword(@Body body: ChangePasswordRequest)

    @PATCH("auth/username")
    suspend fun changeUsername(@Body body: UsernameRequest)

    @POST("auth/mfa/setup")
    suspend fun setupMfa(): MfaSetupResult

    @POST("auth/mfa/confirm")
    suspend fun confirmMfa(@Body body: MfaCodeRequest)

    @POST("auth/mfa/disable")
    suspend fun disableMfa(@Body body: PasswordRequest)

    @POST("auth/delete-account")
    suspend fun deleteAccount(@Body body: PasswordRequest)

    /** `image` is base64 of a small square JPEG. */
    @PUT("users/me/avatar")
    suspend fun uploadAvatar(@Body body: AvatarUploadRequest): AvatarResult

    @DELETE("users/me/avatar")
    suspend fun removeAvatar()

    @POST("reports")
    suspend fun submitReport(@Body body: ReportRequest)

    // --- Public ---

    @GET("public/transparency-feed")
    suspend fun transparencyFeed(@Query("limit") limit: Int): List<TransparencyEventDto>

    // --- Files ---

    @GET("v1/files/config")
    suspend fun fileConfig(): FileConfigDto

    @GET("billing/status")
    suspend fun planStatus(): PlanStatusDto

    /** Switch to Free: the card subscription stops renewing; Pro lasts until the paid period ends. */
    @POST("billing/cancel")
    suspend fun cancelPlan(): PlanStatusDto

    @POST("billing/resume")
    suspend fun resumePlan(): PlanStatusDto

    @GET("v1/files/storage")
    suspend fun fileStorageUsage(): FileStorageUsageDto

    @POST("v1/files/upload-request")
    suspend fun requestFileUpload(@Body body: FileUploadRequest): FileUploadAuthorization

    /** The server insists on an empty JSON object body. */
    @POST("v1/files/{id}/complete")
    suspend fun completeFileUpload(@Path("id") objectId: String, @Body body: JsonObject): FileStatusDto

    /** A file sent on device keys has no server message to attach it to: this makes it downloadable. Uploader only. */
    @POST("v1/files/{id}/release")
    suspend fun releaseFile(@Path("id") objectId: String, @Body body: JsonObject): FileStatusDto

    @GET("v1/files/{id}/download")
    suspend fun fileDownload(@Path("id") objectId: String): FileDownloadDto

    /** Uploader only. */
    @DELETE("v1/files/{id}")
    suspend fun deleteFile(@Path("id") objectId: String)

    // Push notifications: data/api/PushApi.kt.

    // --- Announcements (the team's messages and polls; not end-to-end encrypted) ---

    @GET("announcements")
    suspend fun listAnnouncements(): List<AnnouncementDto>

    /** Only the replies matter here: the item itself is already in the list. */
    @GET("announcements/{id}")
    suspend fun getAnnouncement(@Path("id") id: String): AnnouncementDetailDto

    @POST("announcements/{id}/read")
    suspend fun markAnnouncementRead(@Path("id") id: String)

    @POST("announcements/{id}/vote")
    suspend fun voteAnnouncement(@Path("id") id: String, @Body body: AnnouncementVoteRequest)

    @POST("announcements/{id}/replies")
    suspend fun replyToAnnouncement(@Path("id") id: String, @Body body: AnnouncementReplyRequest): AnnouncementReplyDto

    // --- Blocking (server-enforced) ---

    @GET("blocks")
    suspend fun listBlocked(): List<BlockedUserDto>

    @POST("blocks")
    suspend fun block(@Body body: BlockRequest)

    @DELETE("blocks/{userId}")
    suspend fun unblock(@Path("userId") userId: String)

    // --- Recovery key ---

    @PUT("auth/recovery")
    suspend fun setRecoveryKey(@Body body: SetRecoveryRequest)

    @POST("auth/recovery/start")
    suspend fun recoveryStart(@Body body: RecoveryStartRequest): RecoveryStartResult

    @POST("auth/recovery/complete")
    suspend fun recoveryComplete(@Body body: RecoveryCompleteRequest): LoginResponse

    // --- Groups (server-enforced: friends only; owner/admin manage members) ---

    @POST("conversations")
    suspend fun createConversation(@Body body: CreateConversationRequest): CreatedConversation

    @POST("conversations/{id}/participants")
    suspend fun addParticipant(@Path("id") conversationId: String, @Body body: AddParticipantRequest)

    /** Publishes a group's signed key notice (data/KeyTrust.kt). 409 stale_key_notice: a newer one is already there. */
    @PUT("conversations/{id}/key-notice")
    suspend fun publishKeyNotice(@Path("id") conversationId: String, @Body notice: KeyNoticeDto)

    /** Moves a group to its next key after someone left or was removed (db/migrations/030). */
    @POST("conversations/{id}/keys")
    suspend fun rotateGroupKey(@Path("id") conversationId: String, @Body body: RotateKeyRequest): RotateKeyResult

    /** Registers this account's message-signing public key (set once; the same key again is fine). */
    @PUT("auth/signing-key")
    suspend fun registerSigningKey(@Body body: SigningKeyRequest)

    /** Remove a member (owner/admin), or leave (your own id). */
    @DELETE("conversations/{id}/participants/{userId}")
    suspend fun removeParticipant(@Path("id") conversationId: String, @Path("userId") userId: String)

    @PATCH("conversations/{id}/participants/{userId}/role")
    suspend fun setParticipantRole(@Path("id") conversationId: String, @Path("userId") userId: String, @Body body: RoleRequest)

    @PATCH("conversations/{id}/name")
    suspend fun renameConversation(@Path("id") conversationId: String, @Body body: RenameRequest)

    @GET("friends")
    suspend fun listFriends(): List<FriendDto>
}
