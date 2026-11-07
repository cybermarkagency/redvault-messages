#include "redvault/api/dtos.hpp"

#include <nlohmann/json.hpp>

#include "redvault/api/api_error.hpp"
#include "redvault/util/text.hpp"

namespace redvault::api {

namespace {

// Required: a missing key or wrong type throws json::exception, which the
// API client reports as "malformed_response".
template <typename T>
void req(const Json& j, const char* key, T& out) {
    j.at(key).get_to(out);
}

// Optional with a default: absent or null keeps what's already there.
template <typename T>
void def(const Json& j, const char* key, T& out) {
    const auto it = j.find(key);
    if (it != j.end() && !it->is_null()) it->get_to(out);
}

template <typename T>
void opt(const Json& j, const char* key, std::optional<T>& out) {
    const auto it = j.find(key);
    if (it != j.end() && !it->is_null()) {
        out = it->template get<T>();
    } else {
        out.reset();
    }
}

// Unset optionals are left out of the body entirely.
template <typename T>
void put(Json& j, const char* key, const std::optional<T>& value) {
    if (value) j[key] = *value;
}

/// Ids compare lowercased everywhere, so they're normalized here, where everything else gets them from. An id in
/// capitals from a server would otherwise look like a different person with nothing pinned.
void lowerId(std::string& id) {
    id = util::toLowerAscii(id);
}

/// A number the server named. Anything that isn't a plain integer can't be used, so it's kept as something out of range.
void optKdf(const Json& j, const char* key, std::optional<std::int64_t>& out) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    out = it->is_number_integer() ? it->get<std::int64_t>() : std::int64_t{-1};
}

const std::string& requireField(const std::optional<std::string>& value, const char* name) {
    if (!value) throw ApiError(0, "malformed_response", std::string("Login response is missing ") + name);
    return *value;
}

}  // namespace

LoginResult toLoginResult(const LoginResponse& r) {
    return LoginResult{
        .userId = requireField(r.userId, "userId"),
        .avatarUrl = r.avatarUrl,
        .role = requireField(r.role, "role"),
        .subscriptionTier = requireField(r.subscriptionTier, "subscriptionTier"),
        .publicKey = requireField(r.publicKey, "publicKey"),
        .encryptedPrivateKey = requireField(r.encryptedPrivateKey, "encryptedPrivateKey"),
        .privateKeyNonce = requireField(r.privateKeyNonce, "privateKeyNonce"),
        .privateKeyKdfSalt = requireField(r.privateKeyKdfSalt, "privateKeyKdfSalt"),
        .privateKeyKdfOps = r.privateKeyKdfOps,
        .privateKeyKdfMem = r.privateKeyKdfMem,
    };
}

// --- Responses ---

void from_json(const Json& j, RecoveryStartResult& v) {
    req(j, "publicKey", v.publicKey);
    req(j, "recoveryEncryptedPrivateKey", v.recoveryEncryptedPrivateKey);
    req(j, "recoveryPrivateKeyNonce", v.recoveryPrivateKeyNonce);
    req(j, "mfaRequired", v.mfaRequired);
}

void from_json(const Json& j, AuthResult& v) {
    req(j, "userId", v.userId);
    lowerId(v.userId);
    req(j, "acceptMessageRequests", v.acceptMessageRequests);
    req(j, "transparencyMode", v.transparencyMode);
    opt(j, "avatarUrl", v.avatarUrl);
    req(j, "expiresAt", v.expiresAt);
    req(j, "role", v.role);
    req(j, "subscriptionTier", v.subscriptionTier);
}

void from_json(const Json& j, LoginResponse& v) {
    def(j, "mfaRequired", v.mfaRequired);
    opt(j, "userId", v.userId);
    if (v.userId) lowerId(*v.userId);
    def(j, "acceptMessageRequests", v.acceptMessageRequests);
    def(j, "transparencyMode", v.transparencyMode);
    opt(j, "avatarUrl", v.avatarUrl);
    opt(j, "expiresAt", v.expiresAt);
    opt(j, "role", v.role);
    opt(j, "subscriptionTier", v.subscriptionTier);
    opt(j, "publicKey", v.publicKey);
    opt(j, "encryptedPrivateKey", v.encryptedPrivateKey);
    opt(j, "privateKeyNonce", v.privateKeyNonce);
    opt(j, "privateKeyKdfSalt", v.privateKeyKdfSalt);
    optKdf(j, "privateKeyKdfOps", v.privateKeyKdfOps);
    optKdf(j, "privateKeyKdfMem", v.privateKeyKdfMem);
}

void from_json(const Json& j, MeResult& v) {
    req(j, "userId", v.userId);
    lowerId(v.userId);
    req(j, "username", v.username);
    req(j, "publicKey", v.publicKey);
    req(j, "acceptMessageRequests", v.acceptMessageRequests);
    req(j, "transparencyMode", v.transparencyMode);
    opt(j, "avatarUrl", v.avatarUrl);
    req(j, "role", v.role);
    req(j, "subscriptionTier", v.subscriptionTier);
    req(j, "mfaEnabled", v.mfaEnabled);
    def(j, "hasRecoveryKey", v.hasRecoveryKey);
    def(j, "showOnlineStatus", v.showOnlineStatus);
}

void from_json(const Json& j, ErrorBody& v) {
    opt(j, "error", v.error);
    opt(j, "message", v.message);
}

void from_json(const Json& j, EpochKeyDto& v) {
    req(j, "epoch", v.epoch);
    req(j, "wrappedKey", v.wrappedKey);
}

void from_json(const Json& j, ConversationCommunityDto& v) {
    req(j, "id", v.id);
    req(j, "name", v.name);
    req(j, "kind", v.kind);
    req(j, "status", v.status);
    def(j, "hasAvatar", v.hasAvatar);
    req(j, "role", v.role);
    req(j, "canPost", v.canPost);
    req(j, "keyEpoch", v.keyEpoch);
    req(j, "rekeyRequired", v.rekeyRequired);
}

void from_json(const Json& j, ConversationDto& v) {
    req(j, "id", v.id);
    req(j, "isGroup", v.isGroup);
    req(j, "role", v.role);
    opt(j, "nameCiphertext", v.nameCiphertext);
    opt(j, "nameNonce", v.nameNonce);
    req(j, "wrappedKey", v.wrappedKey);
    opt(j, "disappearingSeconds", v.disappearingSeconds);
    req(j, "createdBy", v.createdBy);
    req(j, "createdAt", v.createdAt);
    def(j, "keyEpoch", v.keyEpoch);
    def(j, "keys", v.keys);
    def(j, "rekeyRequired", v.rekeyRequired);
    opt(j, "community", v.community);
    if (const auto notice = j.find("keyNotice"); notice != j.end() && !notice->is_null()) {
        v.keyNotice = KeyNoticeDto{};
        try {
            from_json(*notice, *v.keyNotice);
        } catch (const std::exception&) {
            v.keyNotice = KeyNoticeDto{};
        }
    }
    lowerId(v.id);
    lowerId(v.createdBy);
}

void from_json(const Json& j, KeyNoticeDto& v) {
    // A fraction would be cut to a whole number here and then sign and verify the same as that number.
    if (!j.at("epoch").is_number_integer() || !j.at("seq").is_number_integer()) throw ApiError(0, "malformed_response", "Bad key notice");
    req(j, "epoch", v.epoch);
    req(j, "seq", v.seq);
    req(j, "issuerId", v.issuerId);
    req(j, "members", v.members);
    req(j, "signature", v.signature);
}

void to_json(Json& j, const KeyNoticeDto& v) {
    j = Json{{"epoch", v.epoch}, {"seq", v.seq}, {"issuerId", v.issuerId}, {"members", v.members}, {"signature", v.signature}};
}

void from_json(const Json& j, ParticipantDto& v) {
    req(j, "userId", v.userId);
    lowerId(v.userId);
    req(j, "role", v.role);
    req(j, "joinedAt", v.joinedAt);
}

void from_json(const Json& j, UserResult& v) {
    req(j, "userId", v.userId);
    lowerId(v.userId);
    req(j, "username", v.username);
    req(j, "publicKey", v.publicKey);
    opt(j, "avatarUrl", v.avatarUrl);
    req(j, "createdAt", v.createdAt);
    def(j, "deleted", v.deleted);
    opt(j, "signingPublicKey", v.signingPublicKey);
    opt(j, "signingKeySince", v.signingKeySince);
}

void from_json(const Json& j, VerificationDto& v) {
    req(j, "publicTransactionId", v.publicTransactionId);
    req(j, "commitment", v.commitment);
    req(j, "protocolVersion", v.protocolVersion);
    req(j, "relayStatus", v.relayStatus);
    req(j, "createdAt", v.createdAt);
    opt(j, "logIndex", v.logIndex);
    opt(j, "sizeKb", v.sizeKb);
}

void from_json(const Json& j, AttachmentDto& v) {
    req(j, "objectId", v.objectId);
    req(j, "status", v.status);
    opt(j, "encryptedSize", v.encryptedSize);
}

void from_json(const Json& j, MessageDto& v) {
    req(j, "id", v.id);
    req(j, "conversationId", v.conversationId);
    lowerId(v.conversationId);
    req(j, "senderId", v.senderId);
    lowerId(v.senderId);
    opt(j, "ciphertext", v.ciphertext);
    opt(j, "nonce", v.nonce);
    req(j, "createdAt", v.createdAt);
    opt(j, "editedAt", v.editedAt);
    opt(j, "deletedAt", v.deletedAt);
    opt(j, "deleteAt", v.deleteAt);
    def(j, "attachments", v.attachments);
    opt(j, "verification", v.verification);
    def(j, "keyEpoch", v.keyEpoch);
    opt(j, "signature", v.signature);
}

void from_json(const Json& j, ReceiptDto& v) {
    req(j, "messageId", v.messageId);
    req(j, "userId", v.userId);
    lowerId(v.userId);
    req(j, "status", v.status);
}

void from_json(const Json& j, UserLookupResult& v) {
    req(j, "userId", v.userId);
    lowerId(v.userId);
    req(j, "publicKey", v.publicKey);
    opt(j, "avatarUrl", v.avatarUrl);
}

void from_json(const Json& j, FriendRequestDto& v) {
    req(j, "id", v.id);
    req(j, "requesterId", v.requesterId);
    lowerId(v.requesterId);
    req(j, "recipientId", v.recipientId);
    lowerId(v.recipientId);
    req(j, "status", v.status);
    req(j, "createdAt", v.createdAt);
    opt(j, "respondedAt", v.respondedAt);
}

void from_json(const Json& j, CreatedConversation& v) {
    req(j, "id", v.id);
    req(j, "isGroup", v.isGroup);
    req(j, "createdAt", v.createdAt);
}

void from_json(const Json& j, MfaSetupResult& v) {
    req(j, "secretBase32", v.secretBase32);
    req(j, "otpauthUri", v.otpauthUri);
}

void from_json(const Json& j, AvatarResult& v) {
    opt(j, "avatarUrl", v.avatarUrl);
}

void from_json(const Json& j, TransparencyEventDto& v) {
    req(j, "publicTransactionId", v.publicTransactionId);
    req(j, "commitment", v.commitment);
    req(j, "protocolVersion", v.protocolVersion);
    req(j, "relayStatus", v.relayStatus);
    req(j, "timestamp", v.timestamp);
    opt(j, "ciphertext", v.ciphertext);
    def(j, "expired", v.expired);
}

void from_json(const Json& j, FileConfigDto& v) {
    req(j, "maxFileSize", v.maxFileSize);
    req(j, "maxEncryptedSize", v.maxEncryptedSize);
    def(j, "maxAttachmentsPerMessage", v.maxAttachmentsPerMessage);
    def(j, "retentionDays", v.retentionDays);
    req(j, "transferMode", v.transferMode);
}

void from_json(const Json& j, FileStorageUsageDto& v) {
    req(j, "used", v.used);
    req(j, "limit", v.limit);
}

void from_json(const Json& j, FilesStorageDto& v) {
    req(j, "free", v.free);
    req(j, "pro", v.pro);
    req(j, "premium", v.premium);
}

void from_json(const Json& j, BillingConfigDto& v) {
    opt(j, "paymentLink", v.paymentLink);
    opt(j, "premiumPaymentLink", v.premiumPaymentLink);
    opt(j, "filesStorage", v.filesStorage);
}

void from_json(const Json& j, PlanStatusDto& v) {
    req(j, "tier", v.tier);
    opt(j, "source", v.source);
    def(j, "renews", v.renews);
    opt(j, "endsOn", v.endsOn);
    def(j, "canCancel", v.canCancel);
    def(j, "canResume", v.canResume);
    opt(j, "canSwitchTo", v.canSwitchTo);
}

void from_json(const Json& j, TransferTargetDto& v) {
    req(j, "mode", v.mode);
    req(j, "url", v.url);
    req(j, "method", v.method);
    def(j, "headers", v.headers);
    req(j, "expiresAt", v.expiresAt);
}

void from_json(const Json& j, FileUploadAuthorization& v) {
    req(j, "objectId", v.objectId);
    req(j, "upload", v.upload);
    req(j, "maxEncryptedSize", v.maxEncryptedSize);
}

void from_json(const Json& j, FileStatusDto& v) {
    req(j, "objectId", v.objectId);
    req(j, "status", v.status);
    opt(j, "encryptedSize", v.encryptedSize);
}

void from_json(const Json& j, FileDownloadDto& v) {
    req(j, "objectId", v.objectId);
    opt(j, "encryptedSize", v.encryptedSize);
    req(j, "download", v.download);
}

void from_json(const Json& j, BlockedUserDto& v) {
    req(j, "userId", v.userId);
    req(j, "username", v.username);
    opt(j, "avatarUrl", v.avatarUrl);
}

void from_json(const Json& j, FriendDto& v) {
    req(j, "userId", v.userId);
    lowerId(v.userId);
    req(j, "username", v.username);
    req(j, "publicKey", v.publicKey);
    opt(j, "avatarUrl", v.avatarUrl);
}

// --- Requests ---

void to_json(Json& j, const RecoveryDto& v) {
    j = Json{{"recoveryAuth", v.recoveryAuth},
             {"recoveryEncryptedPrivateKey", v.recoveryEncryptedPrivateKey},
             {"recoveryPrivateKeyNonce", v.recoveryPrivateKeyNonce}};
}

void to_json(Json& j, const RegisterRequest& v) {
    j = Json{{"username", v.username},
             {"password", v.password},
             {"publicKey", v.publicKey},
             {"encryptedPrivateKey", v.encryptedPrivateKey},
             {"privateKeyNonce", v.privateKeyNonce},
             {"privateKeyKdfSalt", v.privateKeyKdfSalt},
             {"terms_accepted", v.termsAccepted},
             {"privacy_acknowledged", v.privacyAcknowledged},
             {"age_18_confirmed", v.age18Confirmed}};
    put(j, "recovery", v.recovery);
    put(j, "authVersion", v.authVersion);
    put(j, "kdfOps", v.kdfOps);
    put(j, "kdfMem", v.kdfMem);
}

void to_json(Json& j, const SetRecoveryRequest& v) {
    j = Json{{"currentPassword", v.currentPassword},
             {"recoveryAuth", v.recoveryAuth},
             {"recoveryEncryptedPrivateKey", v.recoveryEncryptedPrivateKey},
             {"recoveryPrivateKeyNonce", v.recoveryPrivateKeyNonce}};
}

void to_json(Json& j, const RecoveryStartRequest& v) {
    j = Json{{"username", v.username}, {"recoveryAuth", v.recoveryAuth}};
}

void to_json(Json& j, const RecoveryCompleteRequest& v) {
    j = Json{{"username", v.username}, {"recoveryAuth", v.recoveryAuth}};
    put(j, "code", v.code);
    j["newPassword"] = v.newPassword;
    put(j, "authVersion", v.authVersion);
    j["encryptedPrivateKey"] = v.encryptedPrivateKey;
    j["privateKeyNonce"] = v.privateKeyNonce;
    j["privateKeyKdfSalt"] = v.privateKeyKdfSalt;
    put(j, "kdfOps", v.kdfOps);
    put(j, "kdfMem", v.kdfMem);
}

void to_json(Json& j, const LoginRequest& v) {
    j = Json{{"username", v.username}, {"password", v.password}};
    put(j, "authVersion", v.authVersion);
    put(j, "upgradeAuthKey", v.upgradeAuthKey);
}

void to_json(Json& j, const PreLoginRequest& v) {
    j = Json{{"username", v.username}};
}

void from_json(const Json& j, PreLoginResult& v) {
    j.at("kdfSalt").get_to(v.kdfSalt);
    v.authVersion = j.value("authVersion", 1);
    v.upgrade = j.value("upgrade", false);
    optKdf(j, "kdfOps", v.kdfOps);
    optKdf(j, "kdfMem", v.kdfMem);
}

void to_json(Json& j, const MfaCodeRequest& v) {
    j = Json{{"code", v.code}};
}

void to_json(Json& j, const SendMessageRequest& v) {
    j = Json{{"ciphertext", v.ciphertext}, {"nonce", v.nonce}};
    put(j, "attachments", v.attachments);
    put(j, "keyEpoch", v.keyEpoch);
    put(j, "signature", v.signature);
    put(j, "clientMessageId", v.clientMessageId);
    put(j, "entitlementId", v.entitlementId);
    if (v.safetyContact) j["safetyContact"] = true;
}

void to_json(Json& j, const RotateKeyRequest& v) {
    j = Json{{"fromEpoch", v.fromEpoch}, {"keys", v.keys}};
    put(j, "nameCiphertext", v.nameCiphertext);
    put(j, "nameNonce", v.nameNonce);
    put(j, "keyNotice", v.keyNotice);
}

void to_json(Json& j, const MarkReadRequest& v) {
    j = Json{{"status", v.status}};
}

void to_json(Json& j, const UsernameRequest& v) {
    j = Json{{"username", v.username}};
}

void to_json(Json& j, const WrappedKeyForUser& v) {
    j = Json{{"userId", v.userId}, {"wrappedKey", v.wrappedKey}};
    put(j, "keyEpoch", v.keyEpoch);
    put(j, "keyNotice", v.keyNotice);
}

void to_json(Json& j, const AcceptFriendRequest& v) {
    j = Json{{"participants", v.participants}};
}

void to_json(Json& j, const SettingsRequest& v) {
    j = Json::object();
    put(j, "acceptMessageRequests", v.acceptMessageRequests);
    put(j, "transparencyMode", v.transparencyMode);
    put(j, "showOnlineStatus", v.showOnlineStatus);
}

void to_json(Json& j, const ChangePasswordRequest& v) {
    j = Json{{"currentPassword", v.currentPassword},
             {"newPassword", v.newPassword},
             {"encryptedPrivateKey", v.encryptedPrivateKey},
             {"privateKeyNonce", v.privateKeyNonce},
             {"privateKeyKdfSalt", v.privateKeyKdfSalt}};
    put(j, "authVersion", v.authVersion);
    put(j, "kdfOps", v.kdfOps);
    put(j, "kdfMem", v.kdfMem);
}

void to_json(Json& j, const PasswordRequest& v) {
    j = Json{{"password", v.password}};
}

void to_json(Json& j, const AvatarUploadRequest& v) {
    j = Json{{"image", v.image}};
}

void to_json(Json& j, const ReportRequest& v) {
    j = Json{{"reportedUserId", v.reportedUserId}, {"reason", v.reason}};
}

void to_json(Json& j, const FileUploadRequest& v) {
    j = Json{{"conversationId", v.conversationId},
             {"encryptedSize", v.encryptedSize},
             {"ciphertextSha256", v.ciphertextSha256}};
}

void to_json(Json& j, const BlockRequest& v) {
    j = Json{{"userId", v.userId}};
}

void to_json(Json& j, const CreateConversationRequest& v) {
    j = Json{{"isGroup", v.isGroup}};
    put(j, "nameCiphertext", v.nameCiphertext);
    put(j, "nameNonce", v.nameNonce);
    j["participants"] = v.participants;
}

void to_json(Json& j, const RenameRequest& v) {
    j = Json{{"nameCiphertext", v.nameCiphertext}, {"nameNonce", v.nameNonce}};
}

void to_json(Json& j, const RoleRequest& v) {
    j = Json{{"role", v.role}};
}

}  // namespace redvault::api
