#pragma once

#include <memory>
#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"

namespace redvault::data {

/**
 * Port of the account self-service half of android/.../data/AccountRepository.kt
 * (ui/src/state/actions.ts). Blocking; call from a worker thread.
 */
class AccountRepository {
public:
    explicit AccountRepository(std::shared_ptr<api::ApiClient> api);

    api::MeResult me();

    void setAcceptMessageRequests(bool value);

    /**
     * Re-wraps the (unchanged) private key under the new password on this
     * device and sends it with the password change in one request; changing
     * the password alone would strand the old wrapped key. The server
     * revokes every other session; this one stays signed in.
     */
    void changePassword(const SelfIdentity& identity, const std::string& currentPassword, const std::string& newPassword);

    /** Creates a new recovery key (replacing any old one); returns it formatted, to show once. Needs the password. */
    std::string createRecoveryKey(const SelfIdentity& identity, const std::string& currentPassword);

    /** Step 1 of 2: a pending secret that only takes effect once confirmMfa() gets a valid code, so a bad setup can't lock anyone out. */
    api::MfaSetupResult startMfaSetup();

    void confirmMfa(const std::string& code);

    void disableMfa(const SelfIdentity& identity, const std::string& password);
    void deleteAccount(const SelfIdentity& identity, const std::string& password);

    /** Uploads a profile photo (base64 JPEG, prepared by the caller), which every user can see. Returns its URL. */
    std::optional<std::string> uploadAvatar(const std::string& base64Jpeg);

    void removeAvatar();

private:
    std::shared_ptr<api::ApiClient> api_;
};

}  // namespace redvault::data
