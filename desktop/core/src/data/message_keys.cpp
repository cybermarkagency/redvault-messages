#include "redvault/data/message_keys.hpp"

#include <set>
#include <utility>

#include "redvault/crypto/base64.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

using crypto::base64Decode;
using crypto::base64Encode;

namespace {

constexpr int kMaxRotationAttempts = 3;
/// For a pinned signer the server says nothing about: they sign, so any date counts as after they started.
constexpr const char* kPinnedSince = "0000-00-00";

}  // namespace

const crypto::SecretBytes* KeyRing::key(int epoch) const {
    const auto it = keys.find(epoch);
    return it == keys.end() ? nullptr : &it->second;
}

MessageKeys::MessageKeys(std::shared_ptr<api::ApiClient> api, PinStore pins, std::shared_ptr<KeyPins> keyPins, PinLookup pinned,
                         std::shared_ptr<ContactKeyPins> contacts)
    : api_(std::move(api)),
      pins_(std::move(pins)),
      pinned_(std::move(pinned)),
      trust_(std::make_unique<KeyTrust>(keyPins ? std::move(keyPins) : std::make_shared<MemoryKeyPins>(), std::move(contacts))) {}

void MessageKeys::begin(const SelfIdentity& identity) {
    auto own = std::make_shared<Own>(Own{identity.userId, identity.publicKey, crypto::SecretBytes::copyOf(identity.privateKey),
                                         crypto::deriveSigningKeyPair(identity.privateKey), false, false});
    std::lock_guard lock(mutex_);
    if (!own_ || own_->userId != identity.userId) {
        rings_.clear();
        senderKeys_.clear();
    }
    own_ = std::move(own);
}

void MessageKeys::end() {
    std::lock_guard lock(mutex_);
    own_.reset();
    rings_.clear();
    senderKeys_.clear();
}

std::string MessageKeys::selfId() const {
    std::lock_guard lock(mutex_);
    if (!own_) throw api::UserFacingError("Not signed in");
    return own_->userId;
}

void MessageKeys::openDirect(KeyRing& ring, const api::ConversationDto& dto, const Own& own) {
    if (dto.keyEpoch != 0) {
        // A later epoch isn't believed, and nothing from this answer is pinned.
        // History stays readable only under the key this computer already pinned.
        std::optional<crypto::SecretBytes> first;
        for (const auto& key : dto.keys) {
            if (key.epoch != 0) continue;
            try {
                first = crypto::unsealConversationKey(base64Decode(key.wrappedKey), own.publicKey, own.privateKey);
            } catch (const std::exception&) {
            }
            break;
        }
        if (first && !trust_->matchesPin(own.userId, dto.id, *first)) first.reset();
        ring.keyProblem = trust_->checkDirectKey(own.userId, dto.id, dto.keyEpoch, first ? &*first : nullptr);
        if (first) ring.keys.emplace(0, std::move(*first));
        return;
    }
    auto key = crypto::unsealConversationKey(base64Decode(dto.wrappedKey), own.publicKey, own.privateKey);
    ring.keyProblem = trust_->checkDirectKey(own.userId, dto.id, 0, &key);
    if (!ring.keyProblem) ring.keys.emplace(0, std::move(key));
}

void MessageKeys::openGroup(KeyRing& ring, const api::ConversationDto& dto, const Own& own) {
    ring.currentEpoch = dto.keyEpoch;
    ring.rekeyRequired = dto.rekeyRequired;
    ring.keys.emplace(dto.keyEpoch, crypto::unsealConversationKey(base64Decode(dto.wrappedKey), own.publicKey, own.privateKey));
    for (const auto& key : dto.keys) {
        // Only older keys are history. Nothing else the server lists is a key of this group.
        if (ring.keys.contains(key.epoch) || key.epoch < 0 || key.epoch >= dto.keyEpoch) continue;
        try {
            auto opened = crypto::unsealConversationKey(base64Decode(key.wrappedKey), own.publicKey, own.privateKey);
            // One that isn't what this computer verified at that epoch is left out.
            if (trust_->matchesVerifiedEpoch(own.userId, dto.id, key.epoch, opened)) ring.keys.emplace(key.epoch, std::move(opened));
        } catch (const std::exception&) {
            // An older key that won't open only affects that epoch's messages.
        }
    }
    if (dto.keyNotice) {
        // They may have registered a signing key since this session last asked.
        noteSigned({dto.keyNotice->issuerId});
        resolveSenderKeys(own.userId, {dto.keyNotice->issuerId});
    }
    GroupKeyCheck check;
    check.conversationId = dto.id;
    check.selfUserId = own.userId;
    check.epoch = ring.currentEpoch;
    check.key = ring.currentKey();
    check.notice = &dto.keyNotice;
    check.signingKey = [this](const std::string& userId) { return senderSigningKey(userId); };
    ring.keyProblem = trust_->checkGroupKey(check);
    if (ring.keyProblem) ring.keys.erase(ring.currentEpoch);
}

std::shared_ptr<const KeyRing> MessageKeys::ringFromDto(const api::ConversationDto& dto) {
    std::shared_ptr<Own> own;
    {
        std::lock_guard lock(mutex_);
        own = own_;
    }
    if (!own) throw api::UserFacingError("Not signed in");
    auto ring = std::make_shared<KeyRing>();
    ring->conversationId = dto.id;
    ring->isGroup = dto.isGroup;
    if (dto.isGroup) openGroup(*ring, dto, *own);
    else openDirect(*ring, dto, *own);
    std::lock_guard lock(mutex_);
    rings_[dto.id] = ring;
    return ring;
}

std::shared_ptr<const KeyRing> MessageKeys::ringFor(const std::string& conversationId) const {
    std::lock_guard lock(mutex_);
    const auto it = rings_.find(util::toLowerAscii(conversationId));
    return it == rings_.end() ? nullptr : it->second;
}

std::shared_ptr<const KeyRing> MessageKeys::refresh(const std::string& conversationId) {
    const std::string id = util::toLowerAscii(conversationId);
    for (const auto& dto : api_->listConversations()) {
        if (dto.id == id) return ringFromDto(dto);
    }
    std::lock_guard lock(mutex_);
    rings_.erase(id);
    return nullptr;
}

bool MessageKeys::isPinned(const std::string& conversationId) const {
    std::shared_ptr<Own> own;
    {
        std::lock_guard lock(mutex_);
        own = own_;
    }
    return own && trust_->isPinned(own->userId, conversationId);
}

void MessageKeys::requireUnchangedKey(const std::string& userId, const std::string& publicKeyB64) const {
    trust_->requireUnchangedKey(selfId(), userId, publicKeyB64);
}

std::optional<std::string> MessageKeys::keyProblem(const std::string& conversationId) const {
    const auto ring = ringFor(conversationId);
    return ring ? ring->keyProblem : std::nullopt;
}

std::optional<CurrentKey> MessageKeys::currentKey(const std::string& conversationId) const {
    const auto ring = ringFor(conversationId);
    if (!ring) return std::nullopt;
    if (ring->keyProblem) throw api::UserFacingError(*ring->keyProblem);
    const crypto::SecretBytes* key = ring->currentKey();
    if (key == nullptr) throw api::UserFacingError(KeyTrust::kGroupKeyUnopened);
    return CurrentKey{crypto::SecretBytes::copyOf(*key), ring->currentEpoch};
}

std::optional<api::KeyNoticeDto> MessageKeys::signNotice(const std::string& conversationId, int epoch, crypto::ByteView key,
                                                         const std::vector<std::string>& members,
                                                         const std::optional<api::KeyNoticeDto>& latest) {
    api::KeyNoticeDto notice;
    notice.epoch = epoch;
    crypto::Bytes commitment;
    std::set<std::string> listed;
    for (const auto& member : members) listed.insert(util::toLowerAscii(member));
    notice.members.assign(listed.begin(), listed.end());
    try {
        notice.issuerId = util::toLowerAscii(selfId());
        notice.seq = trust_->nextSeq(notice.issuerId, conversationId, epoch, latest);
        commitment = crypto::keyCommitment(key);
        // Before anything is sent: a chat with no UUIDs (only in tests) has no notices.
        const auto input = crypto::keyNoticeInput(conversationId, notice.issuerId, notice.epoch, notice.seq, notice.members, commitment);
        if (!ensureRegistered()) return std::nullopt;
        std::lock_guard lock(mutex_);
        if (!own_) return std::nullopt;
        notice.signature = base64Encode(crypto::signMessage(input, own_->signing.secretKey));
    } catch (const crypto::CryptoError&) {
        return std::nullopt;
    }
    return notice;
}

std::optional<api::KeyNoticeDto> MessageKeys::noticeAdding(const std::string& conversationId, int epoch, crypto::ByteView key, const std::string& userId) {
    const std::string id = util::toLowerAscii(conversationId);
    const auto dtos = api_->listConversations();
    const api::ConversationDto* dto = nullptr;
    for (const auto& candidate : dtos) {
        if (candidate.id == id && candidate.isGroup) dto = &candidate;
    }
    if (dto == nullptr) throw api::UserFacingError("Not a group you're in.");
    if (const auto ring = ringFromDto(*dto); ring->keyProblem) throw api::UserFacingError(*ring->keyProblem);
    std::vector<std::string> listed;
    for (const auto& participant : api_->listParticipants(id)) listed.push_back(participant.userId);
    const std::string self = selfId();
    return signNotice(id, epoch, key, trust_->membersToSign(self, id, listed, {userId, self}, dto->keyNotice), dto->keyNotice);
}

std::optional<api::KeyNoticeDto> MessageKeys::checkedLatestNotice(const std::string& conversationId, const crypto::SecretBytes& key, const api::KeyNoticeDto& notice) {
    if (notice.issuerId.empty()) return std::nullopt;  // Didn't parse.
    noteSigned({notice.issuerId});
    resolveSenderKeys(selfId(), {notice.issuerId});
    if (!trust_->noticeSignatureOk(conversationId, notice, key, [this](const std::string& userId) { return senderSigningKey(userId); })) return std::nullopt;
    return notice;
}

std::vector<std::string> MessageKeys::membersJoining(const std::string& conversationId, const std::vector<std::string>& listed,
                                                     const std::optional<api::KeyNoticeDto>& latest) {
    const std::string self = selfId();
    return trust_->membersToSign(self, conversationId, listed, {self}, latest);
}

void MessageKeys::adoptNotice(const std::string& conversationId, int epoch, const crypto::SecretBytes& key, const api::KeyNoticeDto& notice) {
    const std::optional<api::KeyNoticeDto> copy = notice;
    GroupKeyCheck check;
    check.conversationId = conversationId;
    check.selfUserId = selfId();
    check.epoch = epoch;
    check.key = &key;
    check.notice = &copy;
    check.signingKey = [this](const std::string& userId) { return senderSigningKey(userId); };
    trust_->checkGroupKey(check);
}

void MessageKeys::publishNotice(const std::string& conversationId, int epoch, const crypto::SecretBytes& key,
                                const std::vector<std::string>& members, const std::optional<api::KeyNoticeDto>& latest) {
    const auto signedNotice = signNotice(conversationId, epoch, key, members, latest);
    if (!signedNotice) return;
    try {
        api_->publishKeyNotice(conversationId, *signedNotice);
    } catch (const api::ApiError& e) {
        // Someone else's is newer: this computer picks that one up on its next load, and pins nothing now.
        if (e.code() == "stale_key_notice") return;
        throw;
    }
    adoptNotice(conversationId, epoch, key, *signedNotice);
}

std::optional<crypto::Bytes> MessageKeys::senderSigningKey(const std::string& userId) const {
    const std::string id = util::toLowerAscii(userId);
    std::lock_guard lock(mutex_);
    if (own_ && util::toLowerAscii(own_->userId) == id) return own_->signing.publicKey;
    const auto it = senderKeys_.find(id);
    if (it == senderKeys_.end()) return std::nullopt;
    return it->second.key;
}

void MessageKeys::rotate(const std::string& conversationId) {
    std::lock_guard rotation(rotation_);
    const std::string id = util::toLowerAscii(conversationId);
    for (int attempt = 0; attempt < kMaxRotationAttempts; ++attempt) {
        const auto dtos = api_->listConversations();
        const api::ConversationDto* dto = nullptr;
        for (const auto& candidate : dtos) {
            if (candidate.id == id && candidate.isGroup) dto = &candidate;
        }
        if (dto == nullptr) throw api::UserFacingError("Not a group you're in.");
        const auto ring = ringFromDto(*dto);
        if (!ring->rekeyRequired) return;  // Someone else already rotated.
        if (ring->keyProblem) throw api::UserFacingError(*ring->keyProblem);
        const crypto::SecretBytes* oldKey = ring->currentKey();
        if (oldKey == nullptr) throw api::UserFacingError(KeyTrust::kGroupKeyUnopened);

        const auto participants = api_->listParticipants(id);
        // The server's list isn't taken on trust: the new key is only sealed to people a signed notice named.
        std::vector<std::string> listed;
        for (const auto& participant : participants) listed.push_back(participant.userId);
        if (!trust_->unconfirmedMembers(selfId(), id, listed).empty()) throw api::UserFacingError(KeyTrust::kUnconfirmedMember);

        crypto::SecretBytes newKey = crypto::generateConversationKey();
        api::RotateKeyRequest request;
        request.fromEpoch = ring->currentEpoch;
        std::vector<std::string> members;
        std::shared_ptr<Own> own;
        {
            std::lock_guard lock(mutex_);
            own = own_;
        }
        for (const auto& participant : participants) {
            const auto user = api_->getUser(participant.userId);
            if (user.deleted) continue;  // A deleted account's key is gone: nothing to seal to.
            // Our own copy goes to our own key: what the server says that is doesn't come into it.
            const bool self = own && util::toLowerAscii(own->userId) == participant.userId;
            if (!self) trust_->requireUnchangedKey(selfId(), participant.userId, user.publicKey);
            const crypto::Bytes recipient = self ? own->publicKey : base64Decode(user.publicKey);
            request.keys.push_back(api::WrappedKeyForUser{
                participant.userId, base64Encode(crypto::sealConversationKey(newKey, recipient)), std::nullopt, std::nullopt});
            members.push_back(participant.userId);
        }
        if (dto->nameCiphertext && dto->nameNonce) {
            const auto name = crypto::decryptWithConversationKey(base64Decode(*dto->nameCiphertext), base64Decode(*dto->nameNonce), *oldKey);
            const auto encrypted = crypto::encryptWithConversationKey(name, newKey);
            request.nameCiphertext = base64Encode(encrypted.ciphertext);
            request.nameNonce = base64Encode(encrypted.nonce);
        }
        request.keyNotice = signNotice(id, ring->currentEpoch + 1, newKey, members);
        int epoch = 0;
        try {
            epoch = api_->rotateGroupKey(id, request);
        } catch (const api::ApiError& e) {
            // Lost a race: look again.
            if (e.code() == "stale_key_epoch" || e.code() == "membership_changed") continue;
            throw;
        }
        auto next = std::make_shared<KeyRing>();
        next->conversationId = id;
        next->isGroup = true;
        next->currentEpoch = epoch;
        for (const auto& [e, key] : ring->keys) next->keys.emplace(e, crypto::SecretBytes::copyOf(key));
        next->keys.emplace(epoch, crypto::SecretBytes::copyOf(newKey));
        {
            std::lock_guard lock(mutex_);
            rings_[id] = std::move(next);
        }
        // Only now: pinning before the server has it would lock the group if the rekey failed.
        if (request.keyNotice) adoptNotice(id, epoch, newKey, *request.keyNotice);
        return;
    }
    throw api::UserFacingError("Couldn't give this group a new key. Try again.");
}

bool MessageKeys::ensureRegistered() {
    std::string publicKey;
    {
        std::lock_guard lock(mutex_);
        if (!own_) return false;
        if (own_->registered || own_->refused) return own_->registered;
        publicKey = base64Encode(own_->signing.publicKey);
    }
    try {
        api_->registerSigningKey(publicKey);
        std::lock_guard lock(mutex_);
        if (!own_) return false;
        own_->registered = true;
        // Our own key is known now: don't keep answering "no key" for our own signed messages.
        const std::string self = util::toLowerAscii(own_->userId);
        const std::string pinned = pins_ ? pins_(self, self, publicKey) : publicKey;
        try {
            SenderKey known;
            known.key = base64Decode(pinned);
            senderKeys_[self] = std::move(known);
        } catch (const std::exception&) {
        }
        return true;
    } catch (const api::ApiError& e) {
        // The server holds a different key for this account (it never should): send unsigned, readers flag it.
        if (e.code() == "signing_key_exists") {
            std::lock_guard lock(mutex_);
            if (own_) own_->refused = true;
        }
        // Otherwise offline or an older server: try again on the next send.
        return false;
    }
}

std::optional<std::string> MessageKeys::signatureFor(const std::string& conversationId, int keyEpoch, crypto::ByteView nonce,
                                                     crypto::ByteView ciphertext) {
    if (!ensureRegistered() || keyEpoch < 0) return std::nullopt;
    std::lock_guard lock(mutex_);
    if (!own_) return std::nullopt;
    try {
        const auto input = crypto::messageSignatureInput(conversationId, own_->userId, static_cast<std::uint32_t>(keyEpoch), nonce, ciphertext);
        return base64Encode(crypto::signMessage(input, own_->signing.secretKey));
    } catch (const crypto::CryptoError&) {
        return std::nullopt;
    }
}

void MessageKeys::resolveSenderKeys(const std::string& selfUserId, const std::vector<std::string>& userIds) {
    // One key per person, whatever the spelling: the pin and the lookup both use the lowercase id.
    const std::string self = util::toLowerAscii(selfUserId);
    std::set<std::string> ids;
    for (const auto& userId : userIds) ids.insert(util::toLowerAscii(userId));
    for (const auto& userId : ids) {
        std::optional<std::string> stored;
        {
            std::lock_guard lock(mutex_);
            if (senderKeys_.contains(userId)) continue;
            // This account's own key is derived here: what the server says about it doesn't matter.
            if (own_ && util::toLowerAscii(own_->userId) == userId) stored = base64Encode(own_->signing.publicKey);
        }
        // Someone who signs doesn't stop: a pinned key holds, whatever the server answers now.
        if (!stored && pinned_) stored = pinned_(self, userId);
        if (stored && stored->empty()) stored.reset();
        std::optional<api::UserResult> user;
        try {
            user = api_->getUser(userId);
        } catch (const std::exception&) {
            // Unknown for now; checked again on the next load.
            if (!stored) continue;
        }
        SenderKey known;
        std::optional<std::string> key = stored;
        if (!key && user && user->signingPublicKey) key = pins_ ? pins_(self, userId, *user->signingPublicKey) : *user->signingPublicKey;
        if (key) {
            try {
                known.key = base64Decode(*key);
            } catch (const std::exception&) {
            }
        }
        known.since = user ? user->signingKeySince : std::nullopt;
        if (!known.since && stored) known.since = kPinnedSince;
        std::lock_guard lock(mutex_);
        senderKeys_[userId] = std::move(known);
    }
}

void MessageKeys::noteSigned(const std::vector<std::string>& senderIds) {
    std::lock_guard lock(mutex_);
    for (const auto& userId : senderIds) {
        const auto it = senderKeys_.find(util::toLowerAscii(userId));
        if (it != senderKeys_.end() && !it->second.key) senderKeys_.erase(it);
    }
}

SenderCheck MessageKeys::checkSender(const api::MessageDto& dto, const std::string& conversationId) const {
    std::lock_guard lock(mutex_);
    const auto it = senderKeys_.find(util::toLowerAscii(dto.senderId));
    const SenderKey* known = it == senderKeys_.end() ? nullptr : &it->second;
    if (dto.signature) {
        if (known == nullptr || !known->key || !dto.nonce || !dto.ciphertext || dto.keyEpoch < 0) return SenderCheck::Invalid;
        try {
            const auto input = crypto::messageSignatureInput(conversationId, dto.senderId, static_cast<std::uint32_t>(dto.keyEpoch),
                                                             base64Decode(*dto.nonce), base64Decode(*dto.ciphertext));
            return crypto::verifyMessageSignature(base64Decode(*dto.signature), input, *known->key) ? SenderCheck::Verified
                                                                                                     : SenderCheck::Invalid;
        } catch (const std::exception&) {
            return SenderCheck::Invalid;
        }
    }
    // Direct chats too: once someone signs, an unsigned message from them is a stripped signature or an old app.
    if (known == nullptr || !known->key || !known->since) return SenderCheck::Legacy;
    return dto.createdAt.substr(0, 10) >= *known->since ? SenderCheck::Unverified : SenderCheck::Legacy;
}

}  // namespace redvault::data
