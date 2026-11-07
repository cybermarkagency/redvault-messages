#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/key_trust.hpp"

namespace redvault::data {

/**
 * One password-manager item, decrypted on this device. `data` holds the
 * item's fields as JSON: {"type": "login"|"passkey"|"card"|"note"|"wifi",
 * "title", "username", "password", "urls": [...], "notes", "totp",
 * "passkey": {...}, "card": {...}, "wifi": {...}, "created", "updated",
 * "passwordChanged"} (times in epoch ms). Everything in it is sealed before
 * it leaves the device.
 */
struct VaultEntry {
    std::string id;
    int revision = 0;
    nlohmann::json data;
    /// Someone else's item shared with this account (read-only).
    bool incoming = false;
    std::string ownerId;
    /// Whom the owner shares it with (user ids). Only for the owner's own items.
    std::vector<std::string> sharedWith;
};

/** The newest revision this computer has seen of each item, per account (lowercase ids), in local settings. */
class VaultRevs {
public:
    virtual ~VaultRevs() = default;
    virtual std::int64_t seen(const std::string& userId, const std::string& name) = 0;
    virtual void record(const std::string& userId, const std::string& name, std::int64_t rev) = 0;
};

/// For tests.
class MemoryVaultRevs final : public VaultRevs {
public:
    std::int64_t seen(const std::string& userId, const std::string& name) override;
    void record(const std::string& userId, const std::string& name, std::int64_t rev) override;

private:
    std::mutex mutex_;
    std::map<std::string, std::int64_t> seen_;
};

/**
 * The password manager's storage: seals items under the vault key before
 * they go to the server, opens them after, keeps shared copies in step with
 * edits, and asks the private breach check. Blocking; run it off the UI thread.
 *
 * `boundId` and `rev` go inside the sealed data, so the server can't swap two
 * records or hand back an older copy: an item in another record's place, or older
 * than this computer has seen, is left out and counted. Nothing is rewritten in
 * the background: an item is bound when next saved, and a new one in a second
 * write because the server picks its id. Shared copies keep their own revision
 * under a separate name. Everything else in an item survives a save untouched.
 */
class VaultRepository {
public:
    /// `revs`: without one they last as long as this object. `contacts`: public key pins, checked before anything is sealed to someone.
    VaultRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const SelfIdentity> identity, std::shared_ptr<VaultRevs> revs = nullptr,
                    std::shared_ptr<ContactKeyPins> contacts = nullptr);

    /// Own items, then items shared with this account.
    std::vector<VaultEntry> list();

    /// How many items the last list() left out, because they aren't what this computer last saw.
    int refused() const { return refused_.load(); }

    /**
     * Creates the item (empty id) or saves a change. Refreshes the copies of
     * everyone it's shared with, except anyone whose safety number changed: their
     * copy waits until that's checked. Throws UserFacingError on a conflict with a
     * change made on another device, asking to reload.
     */
    VaultEntry save(VaultEntry entry);
    void remove(const VaultEntry& entry);
    /// Refuses (UserFacingError) while the recipient's safety number is changed.
    void share(const VaultEntry& entry, const std::string& recipientId);
    void unshare(const VaultEntry& entry, const std::string& recipientId);

    /**
     * How many times this password appears in known breaches (0: none),
     * checked privately: only the first 5 characters of its SHA-1 leave the
     * device. -1 when the check can't be reached.
     */
    std::int64_t breachCount(const std::string& password);

private:
    /// False for an item in another record's place, or older than this computer has seen.
    bool accept(const std::string& id, const nlohmann::json& data, bool shared);
    bool keyUnchanged(const std::string& recipientId, const std::string& publicKeyB64);

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const SelfIdentity> identity_;
    std::shared_ptr<VaultRevs> revs_;
    std::shared_ptr<ContactKeyPins> contacts_;
    std::atomic<int> refused_{0};
};

}  // namespace redvault::data
