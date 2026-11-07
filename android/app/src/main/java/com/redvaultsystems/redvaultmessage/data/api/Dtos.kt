package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

// Wire shapes for the backend, mirroring ui/src/lib/api.ts. Every key,
// nonce and ciphertext is standard padded base64.

@Serializable
data class PreLoginRequest(val username: String)

/** What the client needs to derive its sign-in key. The server answers for any username. */
@Serializable
data class PreLoginResult(
    val kdfSalt: String,
    val authVersion: Int = 1,
    val upgrade: Boolean = false,
    /** Argon2 settings of this account (E2eCrypto.requireSafeKdf checks them); missing on an older server. */
    val kdfOps: Long? = null,
    val kdfMem: Long? = null,
)

@Serializable
data class RegisterRequest(
    val username: String,
    val password: String,
    val authVersion: Int? = null,
    val kdfOps: Long? = null,
    val kdfMem: Long? = null,
    val publicKey: String,
    val encryptedPrivateKey: String,
    val privateKeyNonce: String,
    val privateKeyKdfSalt: String,
    /** Set at sign-up so the account can be recovered with its recovery key. */
    val recovery: RecoveryDto? = null,
    /** The sign-up form's three boxes, as ticked; the server refuses the account unless all are true. */
    @SerialName("terms_accepted") val termsAccepted: Boolean,
    @SerialName("privacy_acknowledged") val privacyAcknowledged: Boolean,
    @SerialName("age_18_confirmed") val age18Confirmed: Boolean,
)

/** Derived from the recovery key; never the key itself. */
@Serializable
data class RecoveryDto(val recoveryAuth: String, val recoveryEncryptedPrivateKey: String, val recoveryPrivateKeyNonce: String)

@Serializable
data class SetRecoveryRequest(
    val currentPassword: String,
    val recoveryAuth: String,
    val recoveryEncryptedPrivateKey: String,
    val recoveryPrivateKeyNonce: String,
)

@Serializable
data class RecoveryStartRequest(val username: String, val recoveryAuth: String)

@Serializable
data class RecoveryStartResult(
    val publicKey: String,
    val recoveryEncryptedPrivateKey: String,
    val recoveryPrivateKeyNonce: String,
    val mfaRequired: Boolean,
)

@Serializable
data class RecoveryCompleteRequest(
    val username: String,
    val recoveryAuth: String,
    val code: String? = null,
    val newPassword: String,
    val authVersion: Int? = null,
    val kdfOps: Long? = null,
    val kdfMem: Long? = null,
    val encryptedPrivateKey: String,
    val privateKeyNonce: String,
    val privateKeyKdfSalt: String,
)

@Serializable
data class LoginRequest(val username: String, val password: String, val authVersion: Int? = null, val upgradeAuthKey: String? = null)

@Serializable
data class MfaCodeRequest(val code: String)

@Serializable
data class AuthResult(
    val userId: String,
    val acceptMessageRequests: Boolean,
    val transparencyMode: Boolean,
    val avatarUrl: String? = null,
    val expiresAt: String,
    val role: String,
    val subscriptionTier: String,
)

/**
 * POST /auth/login answers either `{"mfaRequired": true}` (password was right,
 * but no key material until /auth/mfa/verify succeeds) or a full login
 * result, so every other field is optional here. See [LoginResult].
 */
@Serializable
data class LoginResponse(
    val mfaRequired: Boolean = false,
    val userId: String? = null,
    val acceptMessageRequests: Boolean = true,
    val transparencyMode: Boolean = false,
    val avatarUrl: String? = null,
    val expiresAt: String? = null,
    val role: String? = null,
    val subscriptionTier: String? = null,
    val publicKey: String? = null,
    val encryptedPrivateKey: String? = null,
    val privateKeyNonce: String? = null,
    val privateKeyKdfSalt: String? = null,
    val privateKeyKdfOps: Long? = null,
    val privateKeyKdfMem: Long? = null,
)

/** A completed login (after MFA, if the account has it): identity plus the wrapped private key. */
data class LoginResult(
    val userId: String,
    val avatarUrl: String?,
    val role: String,
    val subscriptionTier: String,
    val publicKey: String,
    val encryptedPrivateKey: String,
    val privateKeyNonce: String,
    val privateKeyKdfSalt: String,
    val privateKeyKdfOps: Long? = null,
    val privateKeyKdfMem: Long? = null,
)

fun LoginResponse.toLoginResult(): LoginResult = LoginResult(
    userId = requireField(userId, "userId"),
    avatarUrl = avatarUrl,
    role = requireField(role, "role"),
    subscriptionTier = requireField(subscriptionTier, "subscriptionTier"),
    publicKey = requireField(publicKey, "publicKey"),
    encryptedPrivateKey = requireField(encryptedPrivateKey, "encryptedPrivateKey"),
    privateKeyNonce = requireField(privateKeyNonce, "privateKeyNonce"),
    privateKeyKdfSalt = requireField(privateKeyKdfSalt, "privateKeyKdfSalt"),
    privateKeyKdfOps = privateKeyKdfOps,
    privateKeyKdfMem = privateKeyKdfMem,
)

private fun requireField(value: String?, name: String): String =
    value ?: throw ApiException(status = 0, code = "malformed_response", message = "Login response is missing $name")

@Serializable
data class MeResult(
    val userId: String,
    val username: String,
    val publicKey: String,
    val acceptMessageRequests: Boolean,
    val transparencyMode: Boolean,
    val avatarUrl: String? = null,
    val role: String,
    val subscriptionTier: String,
    val mfaEnabled: Boolean,
    val hasRecoveryKey: Boolean = false,
    /** Whether contacts see when this account is online (reciprocal: off means you don't see theirs either). */
    val showOnlineStatus: Boolean = false,
)

/** Error body shape for every non-2xx response: `{"error": "<code>", "message": "..."}`. */
@Serializable
data class ErrorBody(val error: String? = null, val message: String? = null)

// --- Conversations and messages ---

@Serializable
data class ConversationDto(
    val id: String,
    val isGroup: Boolean,
    val role: String,
    val nameCiphertext: String? = null,
    val nameNonce: String? = null,
    /** Your sealed copy of the current key ([keyEpoch]); [keys] has every epoch you hold (db/migrations/030). */
    val wrappedKey: String,
    val disappearingSeconds: Int? = null,
    val createdBy: String,
    val createdAt: String,
    val keyEpoch: Int = 0,
    val keys: List<EpochKeyDto> = emptyList(),
    /** Someone left or was removed: a member's app must rotate the key before anything else is sent. */
    val rekeyRequired: Boolean = false,
    /** A group's signed statement of its key and members (data/KeyTrust.kt); null until a member's app publishes one. */
    val keyNotice: KeyNoticeDto? = null,
    /** Present for a paid or free community; the server lists them only because this app asks (?include=communities). */
    val community: CommunityInfoDto? = null,
)

/** A group's signed key notice (data/KeyTrust.kt). Longs, because they're u32 in what's signed. */
@Serializable
data class KeyNoticeDto(val epoch: Long, val seq: Long, val issuerId: String, val members: List<String>, val signature: String)

/** A community conversation's link to its community: who you are there, and its key epoch. */
@Serializable
data class CommunityInfoDto(
    val id: String,
    val name: String,
    /** channel or group */
    val kind: String,
    val status: String,
    val hasAvatar: Boolean = false,
    val role: String,
    /** A channel's subscribers read; the creator and staff post. */
    val canPost: Boolean,
    val keyEpoch: Long,
    val rekeyRequired: Boolean,
)

@Serializable
data class EpochKeyDto(val epoch: Int, val wrappedKey: String)

@Serializable
data class ParticipantDto(val userId: String, val role: String, val joinedAt: String)

@Serializable
data class UserResult(
    val userId: String,
    val username: String,
    val publicKey: String,
    val avatarUrl: String? = null,
    val createdAt: String,
    val deleted: Boolean = false,
    /** Their Ed25519 message-signing key (db/migrations/030); null until one of their updated apps registered it. */
    val signingPublicKey: String? = null,
    /** "YYYY-MM-DD": group messages from them after this day should be signed. */
    val signingKeySince: String? = null,
)

/** Stage-3 "Transaction verified" record. `commitment` is hex SHA-256. */
@Serializable
data class VerificationDto(
    val publicTransactionId: String,
    val commitment: String,
    val protocolVersion: Int,
    val relayStatus: String,
    val createdAt: String,
    /** The message's entry in the transparency log; null until it's logged (or on servers without the log). */
    val logIndex: Long? = null,
)

@Serializable
data class AttachmentDto(val objectId: String, val status: String, val encryptedSize: Long? = null)

@Serializable
data class MessageDto(
    val id: String,
    val conversationId: String,
    val senderId: String,
    // Cleared by the server once a message is deleted.
    val ciphertext: String? = null,
    val nonce: String? = null,
    val createdAt: String,
    val editedAt: String? = null,
    val deletedAt: String? = null,
    /** createdAt + the server's 6-month retention window; null once deleted. */
    val deleteAt: String? = null,
    val attachments: List<AttachmentDto> = emptyList(),
    /** Only on the response to sendMessage; fetch it later with getMessageVerification. */
    val verification: VerificationDto? = null,
    /** Which group key (epoch) the body is encrypted with (a community's epoch key for a community conversation). */
    val keyEpoch: Int = 0,
    /** The sender's Ed25519 signature (E2eCrypto.messageSignatureInput); null from apps that predate signing. */
    val signature: String? = null,
)

/** No timestamp: the server dropped receipts' status_at (db/migrations/005_no_metadata.sql). */
@Serializable
data class ReceiptDto(val messageId: String, val userId: String, val status: String)

/**
 * `attachments`: ids of encrypted files whose descriptors are inside the ciphertext. Omitted for text.
 * `keyEpoch`: community conversations only, the epoch whose key encrypted the body; omitted elsewhere.
 */
@Serializable
data class SendMessageRequest(
    val ciphertext: String,
    val nonce: String,
    val attachments: List<String>? = null,
    /** The group key epoch the body is encrypted with, and the sender's signature over it (db/migrations/030). */
    val keyEpoch: Int? = null,
    val signature: String? = null,
    /** The outbox's id for this send (db/migrations/033): a retry with it returns the message already stored. */
    val clientMessageId: String? = null,
)

/** A group's next key: one sealed copy per current member, and its name re-encrypted with it. */
@Serializable
data class RotateKeyRequest(
    val fromEpoch: Int,
    val keys: List<WrappedKeyForUser>,
    val nameCiphertext: String? = null,
    val nameNonce: String? = null,
    val keyNotice: KeyNoticeDto? = null,
)

@Serializable
data class RotateKeyResult(val keyEpoch: Int)

@Serializable
data class SigningKeyRequest(val signingPublicKey: String)

/** A new group member's sealed copy of the key for [keyEpoch]. */
@Serializable
data class AddParticipantRequest(val userId: String, val wrappedKey: String, val keyEpoch: Int? = null, val keyNotice: KeyNoticeDto? = null)

/** "read" is visible to the sender; "delivered" only keeps the caller's own unread count right. */
@Serializable
data class MarkReadRequest(val status: String)

// --- Friends ---

@Serializable
data class UserLookupResult(val userId: String, val publicKey: String, val avatarUrl: String? = null)

@Serializable
data class FriendRequestDto(
    val id: String,
    val requesterId: String,
    val recipientId: String,
    val status: String,
    val createdAt: String,
    val respondedAt: String? = null,
)

@Serializable
data class UsernameRequest(val username: String)

@Serializable
data class WrappedKeyForUser(val userId: String, val wrappedKey: String)

@Serializable
data class AcceptFriendRequest(val participants: List<WrappedKeyForUser>)

@Serializable
data class CreatedConversation(val id: String, val isGroup: Boolean, val createdAt: String)

// --- Account ---

/** Only the fields being changed are sent (explicitNulls = false drops the rest). */
@Serializable
data class SettingsRequest(
    val acceptMessageRequests: Boolean? = null,
    val transparencyMode: Boolean? = null,
    val showOnlineStatus: Boolean? = null,
)

@Serializable
data class ChangePasswordRequest(
    val currentPassword: String,
    val newPassword: String,
    val authVersion: Int? = null,
    val kdfOps: Long? = null,
    val kdfMem: Long? = null,
    val encryptedPrivateKey: String,
    val privateKeyNonce: String,
    val privateKeyKdfSalt: String,
)

@Serializable
data class MfaSetupResult(val secretBase32: String, val otpauthUri: String)

@Serializable
data class PasswordRequest(val password: String)

@Serializable
data class AvatarUploadRequest(val image: String)

@Serializable
data class AvatarResult(val avatarUrl: String? = null)

@Serializable
data class ReportRequest(val reportedUserId: String, val reason: String)

// --- Public transparency feed (no account needed) ---

/** `ciphertext` is present only for the demo sandbox or a sender who opted in; never plaintext. */
@Serializable
data class TransparencyEventDto(
    val publicTransactionId: String,
    val commitment: String,
    val protocolVersion: Int,
    val relayStatus: String,
    val timestamp: String,
    val ciphertext: String? = null,
    val expired: Boolean = false,
)

// --- End-to-end encrypted files (only ciphertext sizes/hashes and opaque ids) ---

@Serializable
data class FileConfigDto(
    val maxFileSize: Long,
    val maxEncryptedSize: Long,
    val maxAttachmentsPerMessage: Int = 1,
    val retentionDays: Int = 0,
    val transferMode: String,
)

/**
 * Where the plan is headed (server/src/services/billingService.ts's PlanStatus).
 * [endsOn] is the last day of Pro (YYYY-MM-DD) when nothing renews it.
 */
@Serializable
data class PlanStatusDto(
    val tier: String,
    val source: String? = null,
    val renews: Boolean = false,
    val endsOn: String? = null,
    val canCancel: Boolean = false,
    val canResume: Boolean = false,
)

/** Your own storage in use vs. your own plan's limit (0 for Free: sending files is Pro-only). */
@Serializable
data class FileStorageUsageDto(val used: Long, val limit: Long)

@Serializable
data class FileUploadRequest(val conversationId: String, val encryptedSize: Long, val ciphertextSha256: String)

/**
 * Where to move ciphertext. "proxy": `url` is a path relative to the API
 * base, sent with the session cookie. "direct": an absolute presigned HTTPS
 * URL, sent with no cookie at all.
 */
@Serializable
data class TransferTargetDto(
    val mode: String,
    val url: String,
    val method: String,
    val headers: Map<String, String> = emptyMap(),
    val expiresAt: String,
)

@Serializable
data class FileUploadAuthorization(val objectId: String, val upload: TransferTargetDto, val maxEncryptedSize: Long)

@Serializable
data class FileStatusDto(val objectId: String, val status: String, val encryptedSize: Long? = null)

@Serializable
data class FileDownloadDto(val objectId: String, val encryptedSize: Long? = null, val download: TransferTargetDto)

// --- Blocking ---

@Serializable
data class BlockedUserDto(val userId: String, val username: String, val avatarUrl: String? = null)

@Serializable
data class BlockRequest(val userId: String)

// --- Groups ---

@Serializable
data class CreateConversationRequest(
    val isGroup: Boolean,
    val nameCiphertext: String? = null,
    val nameNonce: String? = null,
    val participants: List<WrappedKeyForUser>,
)

@Serializable
data class RenameRequest(val nameCiphertext: String, val nameNonce: String)

@Serializable
data class RoleRequest(val role: String)

@Serializable
data class FriendDto(val userId: String, val username: String, val publicKey: String, val avatarUrl: String? = null)
