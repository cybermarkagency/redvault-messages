// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/model/group_call_keys.hpp"

#include <algorithm>

namespace redvault::model::group_calls {

namespace gc = crypto::groupcalls;

RoomKeyExchange::RoomKeyExchange(Config config) : config_(std::move(config)) {}

std::optional<std::string> RoomKeyExchange::userOfIdentity(const std::string& identity) const {
    for (const auto& m : members_) {
        if (m.identity == identity) return m.userId;
    }
    return std::nullopt;
}

RoomKeyExchange::Outcome RoomKeyExchange::applyKey(crypto::SecretBytes key, std::int64_t epoch) {
    Outcome out;
    keyEpoch_ = std::max(keyEpoch_, epoch);
    if (keyEpoch_ == epoch_) out.clearNotice = true;
    out.install = Outcome::Install{std::move(key), epoch};
    return out;
}

RoomKeyExchange::Outcome RoomKeyExchange::onRoster(std::int64_t epoch, const std::string& keyholder, const std::vector<RosterMember>& members) {
    Outcome out;
    if (epoch <= epoch_) return out;  // a roster we already moved past
    epoch_ = epoch;
    keyholder_ = keyholder;
    members_ = members;
    out.rosterChanged = true;

    std::vector<gc::Participant> known;
    bool complete = true;
    for (const auto& m : members) {
        auto key = config_.publicKeyOf(m.userId);
        if (!key) {
            complete = false;
            continue;
        }
        known.push_back(gc::Participant{m.userId, std::move(*key)});
    }
    out.sas = complete ? gc::groupSasWords(config_.callId, known) : std::vector<std::string>{};

    if (keyholder == config_.selfUserId) {
        // Our turn: a fresh key for this epoch, sealed to everyone else.
        crypto::SecretBytes key = gc::newRoomKey();
        std::vector<api::groupcalls::SealedKey> sealed;
        for (const auto& m : members) {
            if (m.userId == config_.selfUserId) continue;
            const auto publicKey = config_.publicKeyOf(m.userId);
            if (!publicKey) continue;
            const gc::KeyMessage message{config_.callId, config_.conversationId, epoch, config_.selfUserId, m.userId};
            const crypto::calls::Sealed box = gc::sealRoomKey(message, key, *publicKey, config_.selfPrivateKey, config_.now());
            sealed.push_back(api::groupcalls::SealedKey{m.userId, api::live::SealedSignal{box.ciphertext, box.nonce}});
        }
        Outcome applied = applyKey(std::move(key), epoch);
        out.install = std::move(applied.install);
        out.clearNotice = applied.clearNotice;
        out.toSend = std::move(sealed);
        out.sendEpoch = epoch;
        return out;
    }

    // Not the keyholder: a key for this epoch may have arrived before its roster.
    const auto early = early_.find(epoch);
    if (early != early_.end()) {
        EarlyKey held = std::move(early->second);
        early_.erase(early);
        Outcome opened = onKey(epoch, held.from, held.sealed);
        out.install = std::move(opened.install);
        out.notice = std::move(opened.notice);
        out.clearNotice = opened.clearNotice;
    }
    // Older held-over keys can never match a roster any more.
    early_.erase(early_.begin(), early_.upper_bound(epoch));
    return out;
}

RoomKeyExchange::Outcome RoomKeyExchange::onKey(std::int64_t epoch, const std::string& from, const crypto::calls::Sealed& sealed) {
    if (epoch > epoch_) {
        early_[epoch] = EarlyKey{from, sealed};
        return {};
    }
    if (epoch < epoch_ || from != keyholder_) return {};  // a key for an epoch we left, or from someone who isn't the keyholder

    const auto senderKey = config_.publicKeyOf(from);
    try {
        if (!senderKey) throw gc::GroupKeyError("The call key came from someone this app has no identity key for.");
        const gc::KeyMessage expected{config_.callId, config_.conversationId, epoch, from, config_.selfUserId};
        crypto::SecretBytes key = gc::openRoomKey(sealed, *senderKey, config_.selfPrivateKey, expected, config_.now());
        return applyKey(std::move(key), epoch);
    } catch (const gc::GroupKeyError& e) {
        // Not usable: stay silent rather than use a key that didn't verify.
        Outcome out;
        out.notice = e.what();
        return out;
    } catch (const std::exception&) {
        Outcome out;
        out.notice = "Couldn't verify the call key.";
        return out;
    }
}

}  // namespace redvault::model::group_calls
