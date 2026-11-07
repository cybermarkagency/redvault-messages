#pragma once

#include <functional>
#include <optional>
#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

namespace redvault::data {

/**
 * Signing in without giving the server the password (docs/auth-key-split.md).
 * Every password check goes through here, so no path can send it by accident.
 */
struct Credential {
    std::string password;  ///< The sign-in key, or the raw password for an account that hasn't moved yet.
    std::optional<int> authVersion;
    std::optional<std::string> upgradeAuthKey;
};

/// The settings a server names, or the ones every account had before settings were stored. Only a pair counts.
crypto::KdfSettings kdfSettings(const std::optional<std::int64_t>& ops, const std::optional<std::int64_t>& mem);

/// Remembers which accounts this computer saw on sign-in keys. In memory unless the app sets its own.
struct SignInPins {
    std::function<bool(const std::string&)> isPinned;
    std::function<void(const std::string&)> pin;
};
void setSignInPins(SignInPins pins);

/// What to send to prove this password, at sign-in or when an action re-checks it.
Credential credentialFor(api::ApiClient& api, const std::string& handle, const std::string& password);

/// Just the value, for a re-check while signed in.
std::string passwordProof(api::ApiClient& api, const std::string& handle, const std::string& password);

/// What to send for a new password wrapped with `kdfSalt`: at sign-up, password change and recovery.
Credential newCredential(api::ApiClient& api, const std::string& handle, const std::string& password, crypto::ByteView kdfSalt);

/// This computer saw the account on sign-in keys: from now on it refuses to send the password for it. Any spelling of the handle.
void pinSignInKeys(const std::string& handle);

/// The account has a new name: the pin goes with it.
void moveSignInPin(const std::string& oldHandle, const std::string& newHandle);

}  // namespace redvault::data
