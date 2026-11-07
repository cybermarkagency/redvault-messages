#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/group_call_api.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/group_call_crypto.hpp"

namespace redvault::model::group_calls {

/// One person in the call's roster: their account and their pseudonym in the LiveKit room.
struct RosterMember {
    std::string userId;
    std::string identity;
};

/**
 * The room-key protocol of one group call, as a state machine with no I/O: the
 * port of the roster / epoch / key handling in the web app's
 * state/GroupCallContext.tsx (handleRoster, handleKey, applyKey). Feed it the
 * server's events in order; it says what to do about each.
 *
 *   - The server bumps the epoch on every join and leave. The first person
 *     still in the call is the keyholder, who makes a fresh 32-byte key for the
 *     new epoch and seals it to everyone else's identity key.
 *   - Everyone else opens the key from the keyholder, and only from the
 *     keyholder, for exactly the current epoch. A key for an epoch whose roster
 *     hasn't arrived yet is kept until it does.
 *   - Until a key is held, nothing may be published.
 */
class RoomKeyExchange {
public:
    struct Config {
        std::string callId;
        std::string conversationId;
        std::string selfUserId;
        /// Our identity private key (the caller keeps it alive for the life of the call).
        crypto::ByteView selfPrivateKey;
        /// Someone's identity public key as this app knows it from the group's member list; nullopt if unknown.
        std::function<std::optional<crypto::Bytes>(const std::string& userId)> publicKeyOf;
        std::function<std::int64_t()> now = [] { return crypto::calls::nowMs(); };
    };

    /// What an event changes. Everything is optional; apply what is present.
    struct Outcome {
        /// Install this key (in slot epoch % 16), then start publishing if that hasn't happened.
        struct Install {
            crypto::SecretBytes key;
            std::int64_t epoch = 0;
        };
        std::optional<Install> install;
        /// The keyholder's sealed copies for `sendEpoch`, to POST to /keys.
        std::vector<api::groupcalls::SealedKey> toSend;
        std::int64_t sendEpoch = 0;
        /// The safety words changed (empty vector: they can't be worked out yet, an identity key is unknown).
        std::optional<std::vector<std::string>> sas;
        /// A problem worth showing without ending the call.
        std::optional<std::string> notice;
        /// Show no notice any more (a key for the current epoch is now held).
        bool clearNotice = false;
        /// The roster changed: the epoch and members to display.
        bool rosterChanged = false;
    };

    explicit RoomKeyExchange(Config config);

    Outcome onRoster(std::int64_t epoch, const std::string& keyholder, const std::vector<RosterMember>& members);
    Outcome onKey(std::int64_t epoch, const std::string& from, const crypto::calls::Sealed& sealed);

    std::int64_t epoch() const { return epoch_; }
    /// The epoch whose key is held (0: none yet).
    std::int64_t keyEpoch() const { return keyEpoch_; }
    /// A key for the current epoch is held.
    bool keyReady() const { return keyEpoch_ != 0 && keyEpoch_ == epoch_; }
    /// Any key at all is held, so publishing may start.
    bool holdsKey() const { return keyEpoch_ != 0; }
    const std::string& keyholder() const { return keyholder_; }
    const std::vector<RosterMember>& members() const { return members_; }
    /// The LiveKit identity (pseudonym) -> account map of the current roster.
    std::optional<std::string> userOfIdentity(const std::string& identity) const;

private:
    struct EarlyKey {
        std::string from;
        crypto::calls::Sealed sealed;
    };
    Outcome applyKey(crypto::SecretBytes key, std::int64_t epoch);

    Config config_;
    std::int64_t epoch_ = 0;
    std::int64_t keyEpoch_ = 0;
    std::string keyholder_;
    std::vector<RosterMember> members_;
    std::map<std::int64_t, EarlyKey> early_;
};

}  // namespace redvault::model::group_calls
