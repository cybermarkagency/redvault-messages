#include "redvault/data/account_repository.hpp"
#include "redvault/data/sign_in.hpp"

#include <utility>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/recovery_key.hpp"

namespace redvault::data {

using crypto::base64Encode;

AccountRepository::AccountRepository(std::shared_ptr<api::ApiClient> api) : api_(std::move(api)) {}

api::MeResult AccountRepository::me() {
    return api_->me();
}

void AccountRepository::setAcceptMessageRequests(bool value) {
    api_->updateSettings({.acceptMessageRequests = value, .transparencyMode = std::nullopt, .showOnlineStatus = std::nullopt});
}

void AccountRepository::changePassword(const SelfIdentity& identity, const std::string& currentPassword,
                                       const std::string& newPassword) {
    const auto wrapped = crypto::wrapPrivateKey(identity.privateKey, newPassword);
    auto next = newCredential(*api_, identity.handle, newPassword, wrapped.kdfSalt);
    api_->changePassword(api::ChangePasswordRequest{
        .currentPassword = passwordProof(*api_, identity.handle, currentPassword),
        .newPassword = std::move(next.password),
        .authVersion = next.authVersion,
        .encryptedPrivateKey = base64Encode(wrapped.encryptedPrivateKey),
        .privateKeyNonce = base64Encode(wrapped.nonce),
        .privateKeyKdfSalt = base64Encode(wrapped.kdfSalt),
        .kdfOps = wrapped.kdf->ops,
        .kdfMem = wrapped.kdf->mem,
    });
    // The vault's "master password" is the account password: keep checking against the new one.
    if (identity.wrappedKey) *identity.wrappedKey = wrapped;
}

std::string AccountRepository::createRecoveryKey(const SelfIdentity& identity, const std::string& currentPassword) {
    // SecretBytes: the raw key is wiped when this returns; only the formatted copy leaves.
    const auto key = crypto::recovery::generate();
    const auto material = crypto::recovery::materialFor(key, identity.privateKey);
    api_->setRecoveryKey(api::SetRecoveryRequest{
        .currentPassword = passwordProof(*api_, identity.handle, currentPassword),
        .recoveryAuth = material.recoveryAuth,
        .recoveryEncryptedPrivateKey = material.recoveryEncryptedPrivateKey,
        .recoveryPrivateKeyNonce = material.recoveryPrivateKeyNonce,
    });
    return crypto::recovery::format(key);
}

api::MfaSetupResult AccountRepository::startMfaSetup() {
    return api_->setupMfa();
}

void AccountRepository::confirmMfa(const std::string& code) {
    api_->confirmMfa({.code = code});
}

void AccountRepository::disableMfa(const SelfIdentity& identity, const std::string& password) {
    api_->disableMfa({.password = passwordProof(*api_, identity.handle, password)});
}

void AccountRepository::deleteAccount(const SelfIdentity& identity, const std::string& password) {
    api_->deleteAccount({.password = passwordProof(*api_, identity.handle, password)});
}

std::optional<std::string> AccountRepository::uploadAvatar(const std::string& base64Jpeg) {
    return api_->uploadAvatarImage(base64Jpeg);
}

void AccountRepository::removeAvatar() {
    api_->removeAvatar();
}

}  // namespace redvault::data
