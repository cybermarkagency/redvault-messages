// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// Wire shapes for the backend, mirroring android/.../data/api/Dtos.kt (and
// ui/src/lib/api.ts). Every key, nonce and ciphertext is standard padded
// base64. Parsing ignores unknown keys and treats null like an absent key;
// serializing leaves out unset optionals, like the Android client's
// explicitNulls = false.

namespace redvault::api {

using Json = nlohmann::json;

// --- Auth ---

/// Derived from the recovery key; never the key itself.
struct RecoveryDto {
    std::string recoveryAuth;
    std::string recoveryEncryptedPrivateKey;
    std::string recoveryPrivateKeyNonce;
};

struct PreLoginRequest {
    std::string username;
};

/// What the client needs to derive its sign-in key. The server answers for any username.
struct PreLoginResult {
    std::string kdfSalt;
    int authVersion = 1;
    bool upgrade = false;
    /// The account's Argon2 settings. Absent on an older server: those accounts all used the interactive values.
    std::optional<std::int64_t> kdfOps;
    std::optional<std::int64_t> kdfMem;
};

struct RegisterRequest {
    std::string username;
    std::string password;
    std::optional<int> authVersion;
    std::string publicKey;
    std::string encryptedPrivateKey;
    std::string privateKeyNonce;
    std::string privateKeyKdfSalt;
    std::optional<std::int64_t> kdfOps;
    std::optional<std::int64_t> kdfMem;
    /// Set at sign-up so the account can be recovered with its recovery key.
    std::optional<RecoveryDto> recovery;
    /// The sign-up form's three boxes, as ticked; the server refuses the account unless all are true.
    bool termsAccepted = false;
    bool privacyAcknowledged = false;
    bool age18Confirmed = false;
};

struct SetRecoveryRequest {
    std::string currentPassword;
    std::string recoveryAuth;
    std::string recoveryEncryptedPrivateKey;
    std::string recoveryPrivateKeyNonce;
};

struct RecoveryStartRequest {
    std::string username;
    std::string recoveryAuth;
};

struct RecoveryStartResult {
    std::string publicKey;
    std::string recoveryEncryptedPrivateKey;
    std::string recoveryPrivateKeyNonce;
    bool mfaRequired = false;
};

struct RecoveryCompleteRequest {
    std::string username;
    std::string recoveryAuth;
    std::optional<std::string> code;
    std::string newPassword;
    std::optional<int> authVersion;
    std::string encryptedPrivateKey;
    std::string privateKeyNonce;
    std::string privateKeyKdfSalt;
    std::optional<std::int64_t> kdfOps;
    std::optional<std::int64_t> kdfMem;
};

struct LoginRequest {
    std::string username;
    std::string password;
    std::optional<int> authVersion;
    std::optional<std::string> upgradeAuthKey;
};

struct MfaCodeRequest {
    std::string code;
};

struct AuthResult {
    std::string userId;
    bool acceptMessageRequests = true;
    bool transparencyMode = false;
    std::optional<std::string> avatarUrl;
    std::string expiresAt;
    std::string role;
    std::string subscriptionTier;
};

/**
 * POST /auth/login answers either {"mfaRequired": true} (password was right,
 * but no key material until /auth/mfa/verify succeeds) or a full login
 * result, so every other field is optional here. See LoginResult.
 */
struct LoginResponse {
    bool mfaRequired = false;
    std::optional<std::string> userId;
    bool acceptMessageRequests = true;
    bool transparencyMode = false;
    std::optional<std::string> avatarUrl;
    std::optional<std::string> expiresAt;
    std::optional<std::string> role;
    std::optional<std::string> subscriptionTier;
    std::optional<std::string> publicKey;
    std::optional<std::string> encryptedPrivateKey;
    std::optional<std::string> privateKeyNonce;
    std::optional<std::string> privateKeyKdfSalt;
    std::optional<std::int64_t> privateKeyKdfOps;
    std::optional<std::int64_t> privateKeyKdfMem;
};

/// A completed login (after MFA, if the account has it): identity plus the wrapped private key.
struct LoginResult {
    std::string userId;
    std::optional<std::string> avatarUrl;
    std::string role;
    std::string subscriptionTier;
    std::string publicKey;
    std::string encryptedPrivateKey;
    std::string privateKeyNonce;
    std::string privateKeyKdfSalt;
    std::optional<std::int64_t> privateKeyKdfOps;
    std::optional<std::int64_t> privateKeyKdfMem;
};

/// Throws ApiError "malformed_response" if a field a completed login must have is missing.
LoginResult toLoginResult(const LoginResponse& response);

struct MeResult {
    std::string userId;
    std::string username;
    std::string publicKey;
    bool acceptMessageRequests = true;
    bool transparencyMode = false;
    std::optional<std::string> avatarUrl;
    std::string role;
    std::string subscriptionTier;
    bool mfaEnabled = false;
    bool hasRecoveryKey = false;
    /// Opt-in, reciprocal online status (absent on older servers).
    bool showOnlineStatus = false;
};

/// Error body shape for every non-2xx response: {"error": "<code>", "message": "..."}.
struct ErrorBody {
    std::optional<std::string> error;
    std::optional<std::string> message;
};

// --- Conversations and messages ---

/// One group key (epoch) sealed to you (db/migrations/030).
struct EpochKeyDto {
    int epoch = 0;
    std::string wrappedKey;
};

/**
 * A paid or free community riding on a group conversation
 * (docs/paid-communities.md). The server lists these only to apps that ask
 * (`?include=communities`), because their messages use key epochs.
 */
struct ConversationCommunityDto {
    std::string id;
    std::string name;
    /// "channel" (the creator and staff post, members read) or "group" (members talk).
    std::string kind;
    std::string status;
    bool hasAvatar = false;
    /// "owner", "admin", "moderator" or "member".
    std::string role;
    bool canPost = false;
    std::int64_t keyEpoch = 0;
    bool rekeyRequired = false;
};

/**
 * A member's signed statement of a group's key and members (SECURITY_DESIGN_AND_MIGRATION.md, C-2 and H-2;
 * data/key_trust.hpp checks it). One that won't parse is kept as a notice that can't verify, never as no notice.
 */
struct KeyNoticeDto {
    std::int64_t epoch = -1;
    std::int64_t seq = -1;
    std::string issuerId;
    std::vector<std::string> members;
    std::string signature;
};

struct ConversationDto {
    std::string id;
    bool isGroup = false;
    std::string role;
    std::optional<std::string> nameCiphertext;
    std::optional<std::string> nameNonce;
    /// Your sealed copy of the current key (`keyEpoch`); `keys` has every epoch you hold.
    std::string wrappedKey;
    std::optional<int> disappearingSeconds;
    std::string createdBy;
    std::string createdAt;
    int keyEpoch = 0;
    std::vector<EpochKeyDto> keys;
    std::optional<KeyNoticeDto> keyNotice;
    /// Someone left or was removed: a member's app must rotate the key before anything else is sent.
    bool rekeyRequired = false;
    /// Set for a community: its keys are epoch keys (crypto/community_crypto.hpp), not `wrappedKey`.
    std::optional<ConversationCommunityDto> community;
};

struct ParticipantDto {
    std::string userId;
    std::string role;
    std::string joinedAt;
};

struct UserResult {
    std::string userId;
    std::string username;
    std::string publicKey;
    std::optional<std::string> avatarUrl;
    std::string createdAt;
    bool deleted = false;
    /// Their Ed25519 message-signing key (db/migrations/030); unset until one of their updated apps registered it.
    std::optional<std::string> signingPublicKey;
    /// "YYYY-MM-DD": group messages from them after this day should be signed.
    std::optional<std::string> signingKeySince;
};

/// Stage-3 "Transaction verified" record. `commitment` is hex SHA-256.
struct VerificationDto {
    std::string publicTransactionId;
    std::string commitment;
    int protocolVersion = 0;
    std::string relayStatus;
    std::string createdAt;
    /// Its entry in the verifiable transparency log; unset until the next checkpoint logs it.
    std::optional<std::int64_t> logIndex;
    std::optional<int> sizeKb;
};

struct AttachmentDto {
    std::string objectId;
    std::string status;
    std::optional<std::int64_t> encryptedSize;
};

struct MessageDto {
    std::string id;
    std::string conversationId;
    std::string senderId;
    // Cleared by the server once a message is deleted.
    std::optional<std::string> ciphertext;
    std::optional<std::string> nonce;
    std::string createdAt;
    std::optional<std::string> editedAt;
    std::optional<std::string> deletedAt;
    /// createdAt + the server's 6-month retention window; absent once deleted.
    std::optional<std::string> deleteAt;
    std::vector<AttachmentDto> attachments;
    /// Only on the response to sendMessage; fetch it later with getMessageVerification.
    std::optional<VerificationDto> verification;
    /// Which group key (epoch) the body is encrypted with.
    int keyEpoch = 0;
    /// The sender's Ed25519 signature (crypto::messageSignatureInput); unset from apps that predate signing.
    std::optional<std::string> signature;
};

/// No timestamp: the server dropped receipts' status_at (db/migrations/005_no_metadata.sql).
struct ReceiptDto {
    std::string messageId;
    std::string userId;
    std::string status;
};

/// `attachments`: ids of encrypted files whose descriptors are inside the ciphertext. Omitted for text.
struct SendMessageRequest {
    std::string ciphertext;
    std::string nonce;
    std::optional<std::vector<std::string>> attachments;
    /// The group key epoch the body is encrypted with, and the sender's signature over it (db/migrations/030).
    std::optional<int> keyEpoch;
    std::optional<std::string> signature;
    /// The outbox's id for this send (db/migrations/033): a retry with it returns the message already stored.
    std::optional<std::string> clientMessageId;
    /// To someone who charges for messages: the paid message this send uses (api/earnings_api.hpp).
    std::optional<std::string> entitlementId;
    /// The free safety path (one per recipient every 30 days).
    bool safetyContact = false;
};

/// "read" is visible to the sender; "delivered" only keeps the caller's own unread count right.
struct MarkReadRequest {
    std::string status;
};

// --- Friends ---

struct UserLookupResult {
    std::string userId;
    std::string publicKey;
    std::optional<std::string> avatarUrl;
};

struct FriendRequestDto {
    std::string id;
    std::string requesterId;
    std::string recipientId;
    std::string status;
    std::string createdAt;
    std::optional<std::string> respondedAt;
};

struct UsernameRequest {
    std::string username;
};

struct WrappedKeyForUser {
    std::string userId;
    std::string wrappedKey;
    /// Adding a member: which group key `wrappedKey` holds (left out elsewhere).
    std::optional<int> keyEpoch;
    /// Adding a member: the signed notice naming them.
    std::optional<KeyNoticeDto> keyNotice;
};

/// A group's next key: one sealed copy per current member, and its name re-encrypted with it.
struct RotateKeyRequest {
    int fromEpoch = 0;
    std::vector<WrappedKeyForUser> keys;
    std::optional<std::string> nameCiphertext;
    std::optional<std::string> nameNonce;
    std::optional<KeyNoticeDto> keyNotice;
};

struct AcceptFriendRequest {
    std::vector<WrappedKeyForUser> participants;
};

struct CreatedConversation {
    std::string id;
    bool isGroup = false;
    std::string createdAt;
};

// --- Account ---

/// Only the fields being changed are sent.
struct SettingsRequest {
    std::optional<bool> acceptMessageRequests;
    std::optional<bool> transparencyMode;
    std::optional<bool> showOnlineStatus;
};

struct ChangePasswordRequest {
    std::string currentPassword;
    std::string newPassword;
    std::optional<int> authVersion;
    std::string encryptedPrivateKey;
    std::string privateKeyNonce;
    std::string privateKeyKdfSalt;
    std::optional<std::int64_t> kdfOps;
    std::optional<std::int64_t> kdfMem;
};

struct MfaSetupResult {
    std::string secretBase32;
    std::string otpauthUri;
};

struct PasswordRequest {
    std::string password;
};

struct AvatarUploadRequest {
    std::string image;
};

struct AvatarResult {
    std::optional<std::string> avatarUrl;
};

struct ReportRequest {
    std::string reportedUserId;
    std::string reason;
};

// --- Public transparency feed (no account needed) ---

/// `ciphertext` is present only for the demo sandbox or a sender who opted in; never plaintext.
struct TransparencyEventDto {
    std::string publicTransactionId;
    std::string commitment;
    int protocolVersion = 0;
    std::string relayStatus;
    std::string timestamp;
    std::optional<std::string> ciphertext;
    bool expired = false;
};

// --- End-to-end encrypted files (only ciphertext sizes/hashes and opaque ids) ---

struct FileConfigDto {
    std::int64_t maxFileSize = 0;
    std::int64_t maxEncryptedSize = 0;
    int maxAttachmentsPerMessage = 1;
    int retentionDays = 0;
    std::string transferMode;
};

/// Your own storage in use vs. your own plan's limit (0 for Free: sending files is Pro-only).
struct FileStorageUsageDto {
    std::int64_t used = 0;
    std::int64_t limit = 0;
};

/// Each plan's Files (secure backup) storage, in bytes.
struct FilesStorageDto {
    std::int64_t free = 0;
    std::int64_t pro = 0;
    std::int64_t premium = 0;
};

/// Stripe's Pro payment link; absent when the server has billing turned off.
struct BillingConfigDto {
    std::optional<std::string> paymentLink;
    /// Premium (video call links, screen sharing); absent when not offered.
    std::optional<std::string> premiumPaymentLink;
    /// For the plan chooser; absent when the server has no Files (or is older).
    std::optional<FilesStorageDto> filesStorage;
};

/// Where the plan is headed (server/src/services/billingService.ts's PlanStatus).
struct PlanStatusDto {
    std::string tier;
    /// "stripe", "crypto" or "granted"; empty on Free.
    std::optional<std::string> source;
    bool renews = false;
    /// Last day of Pro (YYYY-MM-DD) when nothing renews it.
    std::optional<std::string> endsOn;
    /// A renewing card subscription: offer "Switch to Free".
    bool canCancel = false;
    /// A card subscription set to end: offer "Keep Pro".
    bool canResume = false;
    /// The card subscription can move to this plan ("pro" or "premium") from the app.
    std::optional<std::string> canSwitchTo;
};

struct FileUploadRequest {
    std::string conversationId;
    std::int64_t encryptedSize = 0;
    std::string ciphertextSha256;
};

/**
 * Where to move ciphertext. "proxy": `url` is a path relative to the API
 * base, sent with the session cookie. "direct": an absolute presigned HTTPS
 * URL, sent with no cookie at all.
 */
struct TransferTargetDto {
    std::string mode;
    std::string url;
    std::string method;
    std::map<std::string, std::string> headers;
    std::string expiresAt;
};

struct FileUploadAuthorization {
    std::string objectId;
    TransferTargetDto upload;
    std::int64_t maxEncryptedSize = 0;
};

struct FileStatusDto {
    std::string objectId;
    std::string status;
    std::optional<std::int64_t> encryptedSize;
};

struct FileDownloadDto {
    std::string objectId;
    std::optional<std::int64_t> encryptedSize;
    TransferTargetDto download;
};

// --- Blocking ---

struct BlockedUserDto {
    std::string userId;
    std::string username;
    std::optional<std::string> avatarUrl;
};

struct BlockRequest {
    std::string userId;
};

// --- Groups ---

struct CreateConversationRequest {
    bool isGroup = false;
    std::optional<std::string> nameCiphertext;
    std::optional<std::string> nameNonce;
    std::vector<WrappedKeyForUser> participants;
};

struct RenameRequest {
    std::string nameCiphertext;
    std::string nameNonce;
};

struct RoleRequest {
    std::string role;
};

struct FriendDto {
    std::string userId;
    std::string username;
    std::string publicKey;
    std::optional<std::string> avatarUrl;
};

// Response shapes: parsed from JSON.
void from_json(const Json& j, RecoveryStartResult& v);
void from_json(const Json& j, AuthResult& v);
void from_json(const Json& j, LoginResponse& v);
void from_json(const Json& j, MeResult& v);
void from_json(const Json& j, ErrorBody& v);
void from_json(const Json& j, EpochKeyDto& v);
void from_json(const Json& j, KeyNoticeDto& v);
void to_json(Json& j, const KeyNoticeDto& v);
void from_json(const Json& j, ConversationCommunityDto& v);
void from_json(const Json& j, ConversationDto& v);
void from_json(const Json& j, ParticipantDto& v);
void from_json(const Json& j, UserResult& v);
void from_json(const Json& j, VerificationDto& v);
void from_json(const Json& j, AttachmentDto& v);
void from_json(const Json& j, MessageDto& v);
void from_json(const Json& j, ReceiptDto& v);
void from_json(const Json& j, UserLookupResult& v);
void from_json(const Json& j, FriendRequestDto& v);
void from_json(const Json& j, CreatedConversation& v);
void from_json(const Json& j, MfaSetupResult& v);
void from_json(const Json& j, AvatarResult& v);
void from_json(const Json& j, TransparencyEventDto& v);
void from_json(const Json& j, FileConfigDto& v);
void from_json(const Json& j, FileStorageUsageDto& v);
void from_json(const Json& j, FilesStorageDto& v);
void from_json(const Json& j, BillingConfigDto& v);
void from_json(const Json& j, PlanStatusDto& v);
void from_json(const Json& j, TransferTargetDto& v);
void from_json(const Json& j, FileUploadAuthorization& v);
void from_json(const Json& j, FileStatusDto& v);
void from_json(const Json& j, FileDownloadDto& v);
void from_json(const Json& j, BlockedUserDto& v);
void from_json(const Json& j, FriendDto& v);

// Request shapes: serialized to JSON.
void to_json(Json& j, const RecoveryDto& v);
void to_json(Json& j, const RegisterRequest& v);
void to_json(Json& j, const SetRecoveryRequest& v);
void to_json(Json& j, const RecoveryStartRequest& v);
void to_json(Json& j, const RecoveryCompleteRequest& v);
void to_json(Json& j, const LoginRequest& v);
void to_json(Json& j, const MfaCodeRequest& v);
void to_json(Json& j, const SendMessageRequest& v);
void to_json(Json& j, const RotateKeyRequest& v);
void to_json(Json& j, const MarkReadRequest& v);
void to_json(Json& j, const UsernameRequest& v);
void to_json(Json& j, const WrappedKeyForUser& v);
void to_json(Json& j, const AcceptFriendRequest& v);
void to_json(Json& j, const SettingsRequest& v);
void to_json(Json& j, const ChangePasswordRequest& v);
void to_json(Json& j, const PreLoginRequest& v);
void from_json(const Json& j, PreLoginResult& v);
void to_json(Json& j, const PasswordRequest& v);
void to_json(Json& j, const AvatarUploadRequest& v);
void to_json(Json& j, const ReportRequest& v);
void to_json(Json& j, const FileUploadRequest& v);
void to_json(Json& j, const BlockRequest& v);
void to_json(Json& j, const CreateConversationRequest& v);
void to_json(Json& j, const RenameRequest& v);
void to_json(Json& j, const RoleRequest& v);

}  // namespace redvault::api
