#include "redvault/data/auth_repository.hpp"

#include <sodium.h>

#include <utility>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/recovery_key.hpp"
#include "redvault/data/sign_in.hpp"

namespace redvault::data {

using api::ApiError;
using api::UserFacingError;
using crypto::base64Decode;
using crypto::base64Encode;

namespace {

/// Other people's keys get sealed to this one, and copies of our own: it has to be ours.
void requireOwnPublicKey(const crypto::SecretBytes& privateKey, const crypto::Bytes& publicKey) {
    if (crypto::publicKeyOf(privateKey) != publicKey) throw UserFacingError("This account's keys don't match. Nothing was opened.");
}

}  // namespace

AuthRepository::AuthRepository(std::shared_ptr<api::ApiClient> api) : api_(std::move(api)) {}

Registered AuthRepository::registerAccount(const std::string& handle, const std::string& password, const SignupConsent& consent) {
    auto keyPair = crypto::generateKeyPair();
    const auto wrapped = crypto::wrapPrivateKey(keyPair.privateKey, password);
    // Every account gets a recovery key, shown once right after sign-up.
    const auto recoveryKey = crypto::recovery::generate();
    const auto recovery = crypto::recovery::materialFor(recoveryKey, keyPair.privateKey);

    auto credential = newCredential(*api_, handle, password, wrapped.kdfSalt);
    const bool signInKeys = credential.authVersion.has_value();
    const auto result = api_->registerAccount(api::RegisterRequest{
        .username = handle,
        .password = std::move(credential.password),
        .authVersion = credential.authVersion,
        .publicKey = base64Encode(keyPair.publicKey),
        .encryptedPrivateKey = base64Encode(wrapped.encryptedPrivateKey),
        .privateKeyNonce = base64Encode(wrapped.nonce),
        .privateKeyKdfSalt = base64Encode(wrapped.kdfSalt),
        .kdfOps = wrapped.kdf->ops,
        .kdfMem = wrapped.kdf->mem,
        .recovery = api::RecoveryDto{recovery.recoveryAuth, recovery.recoveryEncryptedPrivateKey,
                                     recovery.recoveryPrivateKeyNonce},
        .termsAccepted = consent.termsAccepted,
        .privacyAcknowledged = consent.privacyAcknowledged,
        .age18Confirmed = consent.age18Confirmed,
    });

    if (signInKeys) pinSignInKeys(handle);
    return Registered{
        .identity =
            SelfIdentity{
                .handle = handle,
                .userId = result.userId,
                .publicKey = std::move(keyPair.publicKey),
                .privateKey = std::move(keyPair.privateKey),
                .avatarUrl = result.avatarUrl,
                .role = result.role,
                .subscriptionTier = result.subscriptionTier,
                .mfaEnabled = false,  // freshly registered accounts never start with MFA on
                .wrappedKey = std::make_shared<crypto::WrappedPrivateKey>(wrapped),
            },
        .recoveryKey = crypto::recovery::format(recoveryKey),
    };
}

RecoverOutcome AuthRepository::recover(const std::string& handle, const std::string& recoveryKeyText,
                                       const std::string& newPassword, const std::optional<std::string>& code) {
    // SecretBytes zeroes the parsed key on every exit path.
    const auto key = crypto::recovery::parse(recoveryKeyText);
    if (!key) throw UserFacingError("That doesn't look like a recovery key. Check it and try again.");

    const std::string auth = crypto::recovery::authFor(*key);
    api::RecoveryStartResult started;
    try {
        started = api_->recoveryStart({.username = handle, .recoveryAuth = auth});
    } catch (const ApiError& e) {
        if (e.code() == "account_locked") throw UserFacingError("Too many tries. Wait a while and try again.");
        if (e.code() == "network_error") throw UserFacingError(e.what());
        throw UserFacingError("That handle and recovery key don't match.");
    }
    const bool haveCode = code && code->find_first_not_of(" \t\r\n") != std::string::npos;
    if (started.mfaRequired && !haveCode) return CodeRequired{};

    crypto::SecretBytes privateKey;
    try {
        privateKey = crypto::recovery::unwrap(*key, started.recoveryEncryptedPrivateKey,
                                              started.recoveryPrivateKeyNonce, started.publicKey);
    } catch (const crypto::CryptoError&) {
        throw UserFacingError("That recovery key doesn't unlock this account.");
    }
    const auto wrapped = crypto::wrapPrivateKey(privateKey, newPassword);
    auto next = newCredential(*api_, handle, newPassword, wrapped.kdfSalt);

    api::LoginResult result;
    try {
        result = api::toLoginResult(api_->recoveryComplete(api::RecoveryCompleteRequest{
            .username = handle,
            .recoveryAuth = auth,
            .code = started.mfaRequired ? code : std::nullopt,
            .newPassword = std::move(next.password),
            .authVersion = next.authVersion,
            .encryptedPrivateKey = base64Encode(wrapped.encryptedPrivateKey),
            .privateKeyNonce = base64Encode(wrapped.nonce),
            .privateKeyKdfSalt = base64Encode(wrapped.kdfSalt),
            .kdfOps = wrapped.kdf->ops,
            .kdfMem = wrapped.kdf->mem,
        }));
    } catch (const ApiError& e) {
        if (e.code() == "invalid_code") {
            throw UserFacingError("Wrong code. Check your authenticator app and try again.");
        }
        throw UserFacingError(e.what());
    }
    if (next.authVersion) pinSignInKeys(handle);
    requireOwnPublicKey(privateKey, base64Decode(result.publicKey));

    return SelfIdentity{
        .handle = handle,
        .userId = result.userId,
        .publicKey = base64Decode(result.publicKey),
        .privateKey = std::move(privateKey),
        .avatarUrl = result.avatarUrl,
        .role = result.role,
        .subscriptionTier = result.subscriptionTier,
        .mfaEnabled = started.mfaRequired,
        .wrappedKey = std::make_shared<crypto::WrappedPrivateKey>(wrapped),
    };
}

LoginOutcome AuthRepository::login(const std::string& handle, const std::string& password) {
    auto credential = credentialFor(*api_, handle, password);
    const bool signInKeys = credential.authVersion.has_value() || credential.upgradeAuthKey.has_value();
    const auto response = api_->login({
        .username = handle,
        .password = std::move(credential.password),
        .authVersion = credential.authVersion,
        .upgradeAuthKey = credential.upgradeAuthKey,
    });
    if (signInKeys) pinSignInKeys(handle);
    if (response.mfaRequired) return MfaRequired{};
    return finishLogin(handle, password, api::toLoginResult(response), false);
}

SelfIdentity AuthRepository::verifyMfa(const std::string& handle, const std::string& password,
                                       const std::string& code) {
    const auto response = api_->verifyMfa({.code = code});
    return finishLogin(handle, password, api::toLoginResult(response), true);
}

void AuthRepository::logout() {
    try {
        api_->logout();
    } catch (const ApiError&) {
        // The client forgets its session regardless.
    }
    api_->cookies().clear();
}

/// Unwraps the private key locally with the just-entered password. The server never sees it unwrapped.
SelfIdentity AuthRepository::finishLogin(const std::string& handle, const std::string& password,
                                         const api::LoginResult& result, bool mfaEnabled) {
    auto wrapped = std::make_shared<crypto::WrappedPrivateKey>(crypto::WrappedPrivateKey{
        .encryptedPrivateKey = base64Decode(result.encryptedPrivateKey),
        .nonce = base64Decode(result.privateKeyNonce),
        .kdfSalt = base64Decode(result.privateKeyKdfSalt),
        .kdf = kdfSettings(result.privateKeyKdfOps, result.privateKeyKdfMem),
    });
    crypto::SecretBytes privateKey;
    try {
        privateKey = crypto::unwrapPrivateKey(*wrapped, password);
    } catch (const crypto::UnsafeKdfError& e) {
        // Refused, so nothing was tried: this isn't a wrong password.
        throw UserFacingError(e.what());
    }
    requireOwnPublicKey(privateKey, base64Decode(result.publicKey));
    return SelfIdentity{
        .handle = handle,
        .userId = result.userId,
        .publicKey = base64Decode(result.publicKey),
        .privateKey = std::move(privateKey),
        .avatarUrl = result.avatarUrl,
        .role = result.role,
        .subscriptionTier = result.subscriptionTier,
        .mfaEnabled = mfaEnabled,
        .wrappedKey = std::move(wrapped),
    };
}

bool SelfIdentity::passwordMatches(const std::string& password) const {
    if (!wrappedKey || wrappedKey->encryptedPrivateKey.empty() || password.empty()) return false;
    try {
        const auto unwrapped = crypto::unwrapPrivateKey(*wrappedKey, password);
        return unwrapped.size() == privateKey.size() && sodium_memcmp(unwrapped.data(), privateKey.data(), privateKey.size()) == 0;
    } catch (const crypto::CryptoError&) {
        return false;
    }
}

}  // namespace redvault::data
