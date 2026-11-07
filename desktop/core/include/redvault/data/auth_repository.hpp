#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "redvault/api/api_client.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

namespace redvault::data {

/**
 * The signed-in user: identity plus the unwrapped private key, held only in
 * memory (SecretBytes) for as long as the session lasts. Move-only, because
 * the private key is.
 */
struct SelfIdentity {
    std::string handle;
    std::string userId;
    crypto::Bytes publicKey;
    crypto::SecretBytes privateKey;
    std::optional<std::string> avatarUrl;
    std::string role;
    std::string subscriptionTier;
    bool mfaEnabled = false;
    /**
     * The private key as the server stores it: wrapped under the account
     * password (useless without it). Kept so the password manager can check
     * the password offline to unlock ("master password"); updated when the
     * password changes.
     */
    std::shared_ptr<crypto::WrappedPrivateKey> wrappedKey = std::make_shared<crypto::WrappedPrivateKey>();

    /// Whether `password` is this account's password (Argon2id: slow, call off the UI thread).
    bool passwordMatches(const std::string& password) const;
};

/// A new account, and the recovery key to show once.
struct Registered {
    SelfIdentity identity;
    std::string recoveryKey;
};

/// Password was right, but the account has MFA: call AuthRepository::verifyMfa next.
struct MfaRequired {};

/// The account has two-factor on: ask for the code and try again. Nothing changed yet.
struct CodeRequired {};

using LoginOutcome = std::variant<SelfIdentity, MfaRequired>;
using RecoverOutcome = std::variant<SelfIdentity, CodeRequired>;

/// What the person ticked on the sign-up form, sent as is: the server decides.
struct SignupConsent {
    bool termsAccepted = false;
    bool privacyAcknowledged = false;
    bool age18Confirmed = false;

    bool complete() const { return termsAccepted && privacyAcknowledged && age18Confirmed; }
};

/**
 * Port of android/.../data/AuthRepository.kt (itself the auth half of
 * ui/src/state/actions.ts). Every method blocks, and key generation,
 * Argon2id and unwrapping are deliberately slow, so the UI must call these
 * from a worker thread.
 *
 * Remembering the last handle is left to the caller (it's a UI preference),
 * as is wiping: dropping a SelfIdentity zeroes its private key.
 */
class AuthRepository {
public:
    explicit AuthRepository(std::shared_ptr<api::ApiClient> api);

    Registered registerAccount(const std::string& handle, const std::string& password, const SignupConsent& consent);

    LoginOutcome login(const std::string& handle, const std::string& password);

    /// Completes a login that returned MfaRequired, with the same handle and password.
    SelfIdentity verifyMfa(const std::string& handle, const std::string& password, const std::string& code);

    /**
     * Recovers an account with its recovery key: unwraps the private key on
     * this device (checking it matches the account), re-wraps it under the
     * new password, and signs in. Every other session is signed out.
     * Failures a person can act on throw api::UserFacingError.
     */
    RecoverOutcome recover(const std::string& handle, const std::string& recoveryKeyText,
                           const std::string& newPassword, const std::optional<std::string>& code);

    /// Best-effort server logout; the session cookie is cleared either way.
    void logout();

private:
    SelfIdentity finishLogin(const std::string& handle, const std::string& password, const api::LoginResult& result,
                             bool mfaEnabled);

    std::shared_ptr<api::ApiClient> api_;
};

}  // namespace redvault::data
