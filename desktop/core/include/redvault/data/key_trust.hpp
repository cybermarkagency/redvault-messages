#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/dtos.hpp"
#include "redvault/crypto/bytes.hpp"

// Trust in protocol 1 conversation keys: port of ui/src/lib/keyTrust.ts
// (remediation/SECURITY_DESIGN_AND_MIGRATION.md, C-2 and H-2).
//
// crypto_box_seal says nothing about who sealed a key, so the server could
// hand out a key of its own. Two rules stop that:
//   - A direct chat's key never changes: this computer pins the key it first
//     saw, and a new epoch or a different key is refused.
//   - A group's key comes with a key notice, signed by the member who issued
//     it, naming the conversation, the epoch, the members and a commitment to
//     the key. A new epoch's issuer must be in the member list this computer
//     last verified, and epochs and sequence numbers never go backwards.
//
// First contact is still trust on first use: a computer has nothing earlier to
// compare with. Pins are per account and per computer.

namespace redvault::data {

/// What this computer last verified about a conversation's key.
struct KeyPin {
    std::int64_t epoch = 0;
    std::int64_t seq = 0;
    /// base64.
    std::string commitment;
    /// Lowercase.
    std::vector<std::string> members;
    /// Commitments to the epochs before `epoch` that this computer verified, so they can't be swapped later.
    std::map<std::int64_t, std::string> past;
};

/// Every id is lowercased before it reaches a store, so no spelling of an id can have a pin of its own.
class KeyPins {
public:
    virtual ~KeyPins() = default;
    virtual std::optional<KeyPin> load(const std::string& selfUserId, const std::string& conversationId) = 0;
    virtual void save(const std::string& selfUserId, const std::string& conversationId, const KeyPin& pin) = 0;
};

/// Pins for one session, with nothing kept: for tests and the CLI.
class MemoryKeyPins final : public KeyPins {
public:
    std::optional<KeyPin> load(const std::string& selfUserId, const std::string& conversationId) override;
    void save(const std::string& selfUserId, const std::string& conversationId, const KeyPin& pin) override;

private:
    std::mutex mutex_;
    std::map<std::string, KeyPin> pins_;
};

/// Pins kept as JSON text in some store (the app's: local settings). Text that doesn't parse reads as no pin.
class StoredKeyPins final : public KeyPins {
public:
    using Read = std::function<std::string(const std::string& selfUserId, const std::string& conversationId)>;
    using Write = std::function<void(const std::string& selfUserId, const std::string& conversationId, const std::string& json)>;

    StoredKeyPins(Read read, Write write);
    std::optional<KeyPin> load(const std::string& selfUserId, const std::string& conversationId) override;
    void save(const std::string& selfUserId, const std::string& conversationId, const KeyPin& pin) override;

private:
    Read read_;
    Write write_;
};

/**
 * The first public key seen for each contact (the same pin as the safety number warning). The server hands out public
 * keys, so one that differs from what this computer has seen would seal a group key, or a shared item, to whoever
 * holds the other half.
 */
class ContactKeyPins {
public:
    virtual ~ContactKeyPins() = default;
    /// Remembers the first key seen for this contact; true when this one differs.
    virtual bool keyChanged(const std::string& selfUserId, const std::string& contactUserId, const std::string& publicKeyB64) = 0;
};

/// For tests.
class MemoryContactKeyPins final : public ContactKeyPins {
public:
    bool keyChanged(const std::string& selfUserId, const std::string& contactUserId, const std::string& publicKeyB64) override;

private:
    std::mutex mutex_;
    std::map<std::string, std::string> pins_;
};

/// Call before sealing anything to someone. Throws UserFacingError with KeyTrust::kContactKeyChanged. No pins (`contacts` null): nothing to compare.
void requireUnchangedKey(ContactKeyPins* contacts, const std::string& selfUserId, const std::string& contactUserId, const std::string& publicKeyB64);

/// What checkGroupKey is given about a group's current key.
struct GroupKeyCheck {
    std::string conversationId;
    std::string selfUserId;
    std::int64_t epoch = 0;
    /// The key the server's copy opens to, for `epoch`, or nullptr.
    const crypto::SecretBytes* key = nullptr;
    const std::optional<api::KeyNoticeDto>* notice = nullptr;
    /// The pinned signing key of a user (given lowercase), or nullopt when unknown.
    std::function<std::optional<crypto::Bytes>(const std::string& userId)> signingKey;
};

/// Thread-safe. Don't call it while holding a lock that `signingKey` needs.
class KeyTrust {
public:
    static constexpr const char* kDirectKeyChanged = "This chat's key was changed, which should never happen. Nothing can be sent.";
    static constexpr const char* kDirectKeyUnopened = "This chat's key couldn't be opened.";
    static constexpr const char* kGroupKeyUnverified = "This group's key couldn't be verified. Nothing can be sent.";
    static constexpr const char* kGroupKeyUnopened = "This group's key couldn't be opened.";
    static constexpr const char* kUnconfirmedMember = "This group lists someone no member confirmed, so its key wasn't changed.";
    static constexpr const char* kContactKeyChanged = "Someone's safety number changed, so nothing was sent to them. Check it in their chat first.";

    explicit KeyTrust(std::shared_ptr<KeyPins> pins, std::shared_ptr<ContactKeyPins> contacts = nullptr);

    /// See the free function: this one uses the contact pins this object was given.
    void requireUnchangedKey(const std::string& selfUserId, const std::string& contactUserId, const std::string& publicKeyB64);

    /// Whether this computer has pinned this conversation's key: then it isn't a community, whatever the server says now.
    bool isPinned(const std::string& selfUserId, const std::string& conversationId);

    /// What's wrong with a direct chat's key, or nullopt. Pins the key the first time, and only at epoch 0.
    std::optional<std::string> checkDirectKey(const std::string& selfUserId, const std::string& conversationId, std::int64_t epoch,
                                              const crypto::SecretBytes* key);

    /// Whether `key` is the one pinned for this chat. Nothing pinned, nothing to vouch for it.
    bool matchesPin(const std::string& selfUserId, const std::string& conversationId, const crypto::SecretBytes& key);

    /// What's wrong with a group's current key, or nullopt. Updates the pin only when the notice checks out.
    std::optional<std::string> checkGroupKey(const GroupKeyCheck& check);

    /**
     * Whether a member who held `key` signed this notice. The signature covers a commitment to the key that's never
     * sent, so nobody without the key can make one that passes, even with a signing key of their own.
     */
    bool noticeSignatureOk(const std::string& conversationId, const api::KeyNoticeDto& notice, const crypto::SecretBytes& key,
                           const std::function<std::optional<crypto::Bytes>(const std::string&)>& signingKey) const;

    /// False for a key that isn't the one this computer verified at `epoch`. An epoch it never verified can't be checked.
    bool matchesVerifiedEpoch(const std::string& selfUserId, const std::string& conversationId, std::int64_t epoch, const crypto::SecretBytes& key);

    /**
     * Who a new notice names. The server's list isn't taken on trust: anyone it added that no notice named would
     * otherwise be confirmed by someone else's routine change. So it's the last verified list, or `latest` for someone
     * joining who has none of their own, less whoever the server no longer lists, plus `added`.
     */
    std::vector<std::string> membersToSign(const std::string& selfUserId, const std::string& conversationId, const std::vector<std::string>& listed,
                                           const std::vector<std::string>& added, const std::optional<api::KeyNoticeDto>& latest);

    /// People the server lists who aren't in the last verified notice. A rotation mustn't seal the key to them.
    std::vector<std::string> unconfirmedMembers(const std::string& selfUserId, const std::string& conversationId,
                                                const std::vector<std::string>& participants);

    /// The sequence number for this computer's next notice at `epoch`: past what it pinned and past `latest`, the server's.
    std::int64_t nextSeq(const std::string& selfUserId, const std::string& conversationId, std::int64_t epoch,
                         const std::optional<api::KeyNoticeDto>& latest);

    /// What's pinned for a conversation, for tests and diagnostics.
    std::optional<KeyPin> pin(const std::string& selfUserId, const std::string& conversationId);

private:
    std::mutex mutex_;
    std::shared_ptr<KeyPins> pins_;
    std::shared_ptr<ContactKeyPins> contacts_;
};

}  // namespace redvault::data
