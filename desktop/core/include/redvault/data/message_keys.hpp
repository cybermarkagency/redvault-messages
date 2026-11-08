// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/key_trust.hpp"

namespace redvault::data {

/// One conversation's unsealed keys by epoch (db/migrations/030).
struct KeyRing {
    std::string conversationId;
    bool isGroup = false;
    int currentEpoch = 0;
    std::map<int, crypto::SecretBytes> keys;
    bool rekeyRequired = false;
    /// Set when the key failed a trust check (data/key_trust.hpp): nothing may be sent, and that key isn't in `keys`.
    std::optional<std::string> keyProblem;

    /// The key for `epoch`, or nullptr when this session doesn't hold it.
    const crypto::SecretBytes* key(int epoch) const;
    /// Nullptr when the key was refused.
    const crypto::SecretBytes* currentKey() const { return key(currentEpoch); }
};

/// The key to encrypt with now, and its epoch.
struct CurrentKey {
    crypto::SecretBytes key;
    int epoch = 0;
};

/// How far a message's sender can be trusted; see MessageKeys::checkSender.
enum class SenderCheck { Verified, Legacy, Unverified, Invalid };

/**
 * Port of android/.../data/MessageKeys.kt (ui/src/lib/groupKeys.ts and
 * ui/src/lib/messageSigning.ts; docs/group-keys-and-signatures.md):
 *
 * - Group key epochs. When someone leaves or is removed, the server takes no
 *   messages until a remaining member's app makes a fresh random key for the
 *   next epoch and seals it to every remaining member with crypto_box_seal.
 *   Older epochs' keys stay, so history stays readable.
 * - Message signatures. Each account signs every message with the Ed25519 key
 *   derived from its private key; readers check it against the sender's key,
 *   pinned on first sight.
 * - Key trust. No key is used until KeyTrust has accepted it: the rings other
 *   code sees have been checked, and a refused key isn't in them.
 *
 * Every user id is lowercased before it's looked up or pinned, here and in
 * KeyTrust, so an id in capitals from the server can't look like a new person
 * with nothing pinned.
 *
 * Thread-safe; the network calls block, so call from a worker thread.
 */
class MessageKeys {
public:
    /// Stores `keyB64` as `userId`'s signing key unless one is already pinned; returns the pinned key.
    using PinStore = std::function<std::string(const std::string& selfUserId, const std::string& userId, const std::string& keyB64)>;
    /// The signing key pinned for someone, if any, without pinning anything.
    using PinLookup = std::function<std::optional<std::string>(const std::string& selfUserId, const std::string& userId)>;

    /**
     * `keyPins`: where key pins are kept. Without one they last as long as this object (tests, the CLI).
     * `pinned`: reads the pins `pins` keeps, so a pinned signer stays one whatever the server says.
     * `contacts`: public key pins, checked before anything is sealed to someone.
     */
    MessageKeys(std::shared_ptr<api::ApiClient> api, PinStore pins, std::shared_ptr<KeyPins> keyPins = nullptr, PinLookup pinned = {},
                std::shared_ptr<ContactKeyPins> contacts = nullptr);

    /// Called when a session starts. Keeps copies of what it needs from `identity`.
    void begin(const SelfIdentity& identity);
    /// Called when the session ends: nothing of it stays in memory.
    void end();

    /**
     * Opens a conversation's keys, checks them and remembers the ring. Throws only if the current key can't be opened.
     * A key that fails its check is left out and KeyRing::keyProblem says why: a substituted key can neither read
     * what's sent nor forge what's shown. A direct chat only ever has epoch 0, so nothing else is even opened.
     */
    std::shared_ptr<const KeyRing> ringFromDto(const api::ConversationDto& dto);
    std::shared_ptr<const KeyRing> ringFor(const std::string& conversationId) const;
    /// Re-reads one conversation's keys (someone else rotated). Null when it's gone.
    std::shared_ptr<const KeyRing> refresh(const std::string& conversationId);

    /**
     * Moves a group to its next key. Any member may; when two do at once the
     * server keeps one and the other picks up its key. The new key is sealed only to
     * people a verified notice named: anyone else on the server's list stops it, with
     * KeyTrust::kUnconfirmedMember. Throws on failure.
     */
    void rotate(const std::string& conversationId);

    /// Why this chat's key was refused, when it was.
    std::optional<std::string> keyProblem(const std::string& conversationId) const;

    /// Whether this computer has pinned this conversation's key: then it isn't a community, whatever the server says now.
    bool isPinned(const std::string& conversationId) const;

    /// Call before sealing anything to someone. Throws UserFacingError with KeyTrust::kContactKeyChanged.
    void requireUnchangedKey(const std::string& userId, const std::string& publicKeyB64) const;

    /// The key to encrypt with now. Nullopt for a conversation not loaded yet; UserFacingError with the reason when its key was refused.
    std::optional<CurrentKey> currentKey(const std::string& conversationId) const;

    /// A signed notice for this key and these members, or nullopt until this account's signing key is registered.
    std::optional<api::KeyNoticeDto> signNotice(const std::string& conversationId, int epoch, crypto::ByteView key,
                                                const std::vector<std::string>& members,
                                                const std::optional<api::KeyNoticeDto>& latest = std::nullopt);

    /**
     * The notice for adding someone to a group. Reads the latest notice first, because naming an out-of-date list
     * would leave out whoever joined since. Nullopt until this account's signing key is registered. Throws with the
     * reason when the group's key was refused.
     */
    std::optional<api::KeyNoticeDto> noticeAdding(const std::string& conversationId, int epoch, crypto::ByteView key, const std::string& userId);

    /**
     * The group's latest notice, for someone who has just joined and has no pin of their own, or nullopt if no member
     * who held this key signed it. The joiner names its members, so an unchecked list would vouch for the server's.
     */
    std::optional<api::KeyNoticeDto> checkedLatestNotice(const std::string& conversationId, const crypto::SecretBytes& key, const api::KeyNoticeDto& notice);

    /// Who a joiner's notice names: see KeyTrust::membersToSign.
    std::vector<std::string> membersJoining(const std::string& conversationId, const std::vector<std::string>& listed,
                                            const std::optional<api::KeyNoticeDto>& latest);

    /// Pins a notice this computer published. Only after the server has it: pinning first would lock the group if the send failed.
    void adoptNotice(const std::string& conversationId, int epoch, const crypto::SecretBytes& key, const api::KeyNoticeDto& notice);

    /**
     * Publishes a signed notice for the group's current key and these members: once for a group that has none yet,
     * and by someone who joined through a link. A client that loses the race (stale_key_notice) has nothing to redo,
     * and pins nothing: it picks up the newer notice on its next load. Other failures are thrown, for the caller to ignore.
     */
    void publishNotice(const std::string& conversationId, int epoch, const crypto::SecretBytes& key, const std::vector<std::string>& members,
                       const std::optional<api::KeyNoticeDto>& latest = std::nullopt);

    /// The pinned signing key of someone already looked up. This account's own is derived here, so what the server says about it doesn't matter.
    std::optional<crypto::Bytes> senderSigningKey(const std::string& userId) const;

    KeyTrust& trust() { return *trust_; }

    /// This account's signature (base64) over a message about to be sent, or nullopt when signing isn't set up (yet).
    std::optional<std::string> signatureFor(const std::string& conversationId, int keyEpoch, crypto::ByteView nonce,
                                            crypto::ByteView ciphertext);

    /// Makes sure these senders' signing keys are known; the first one seen for each is pinned.
    void resolveSenderKeys(const std::string& selfUserId, const std::vector<std::string>& userIds);

    /**
     * These people just sent signed messages. Anyone we hold no key for is looked up again by the next
     * resolveSenderKeys: they registered a key after we last asked (their first send, or ours). Without this,
     * a "no key yet" answer stayed cached for the whole session and their signed messages read as forged.
     */
    void noteSigned(const std::vector<std::string>& senderIds);

    /**
     * Verified: a valid signature by the sender's pinned key. Legacy: unsigned,
     * from before the sender had a key. Unverified: an unsigned message from
     * someone whose apps sign, sent after they started (an older app of theirs,
     * a stripped signature or a forgery), in a group or a direct chat: shown with
     * a warning, and it can't edit, react, vote or pin. Invalid: a signature that
     * doesn't check out.
     *
     * `conversationId` is the chat being read: the message's own claim to one isn't believed.
     */
    SenderCheck checkSender(const api::MessageDto& dto, const std::string& conversationId) const;

private:
    struct Own {
        std::string userId;
        crypto::Bytes publicKey;
        crypto::SecretBytes privateKey;
        crypto::SigningKeyPair signing;
        bool registered = false;
        bool refused = false;
    };
    struct SenderKey {
        std::optional<crypto::Bytes> key;
        std::optional<std::string> since;
    };

    void openDirect(KeyRing& ring, const api::ConversationDto& dto, const Own& own);
    void openGroup(KeyRing& ring, const api::ConversationDto& dto, const Own& own);
    bool ensureRegistered();
    std::string selfId() const;

    std::shared_ptr<api::ApiClient> api_;
    PinStore pins_;
    PinLookup pinned_;
    mutable std::mutex mutex_;
    std::mutex rotation_;
    std::shared_ptr<Own> own_;
    std::unique_ptr<KeyTrust> trust_;
    std::unordered_map<std::string, std::shared_ptr<const KeyRing>> rings_;
    std::unordered_map<std::string, SenderKey> senderKeys_;
};

}  // namespace redvault::data
