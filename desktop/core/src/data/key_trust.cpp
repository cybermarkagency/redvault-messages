#include "redvault/data/key_trust.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

using crypto::base64Decode;
using crypto::base64Encode;
using util::toLowerAscii;

namespace {

using Json = nlohmann::json;

std::string pinName(const std::string& selfUserId, const std::string& conversationId) {
    return toLowerAscii(selfUserId) + ":" + toLowerAscii(conversationId);
}

std::vector<std::string> lowered(const std::vector<std::string>& ids) {
    std::vector<std::string> out;
    out.reserve(ids.size());
    for (const auto& id : ids) out.push_back(toLowerAscii(id));
    return out;
}

bool contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool sameMembers(std::vector<std::string> a, std::vector<std::string> b) {
    std::sort(a.begin(), a.end());
    a.erase(std::unique(a.begin(), a.end()), a.end());
    std::sort(b.begin(), b.end());
    b.erase(std::unique(b.begin(), b.end()), b.end());
    return a == b;
}

/// Base64, or empty for a key that isn't a key.
std::string commitmentOf(const crypto::SecretBytes& key) {
    try {
        return base64Encode(crypto::keyCommitment(key));
    } catch (const crypto::CryptoError&) {
        return {};
    }
}

}  // namespace

std::optional<KeyPin> MemoryKeyPins::load(const std::string& selfUserId, const std::string& conversationId) {
    std::lock_guard lock(mutex_);
    const auto it = pins_.find(pinName(selfUserId, conversationId));
    if (it == pins_.end()) return std::nullopt;
    return it->second;
}

void MemoryKeyPins::save(const std::string& selfUserId, const std::string& conversationId, const KeyPin& pin) {
    std::lock_guard lock(mutex_);
    pins_[pinName(selfUserId, conversationId)] = pin;
}

StoredKeyPins::StoredKeyPins(Read read, Write write) : read_(std::move(read)), write_(std::move(write)) {}

std::optional<KeyPin> StoredKeyPins::load(const std::string& selfUserId, const std::string& conversationId) {
    const std::string text = read_(toLowerAscii(selfUserId), toLowerAscii(conversationId));
    if (text.empty()) return std::nullopt;
    try {
        const Json j = Json::parse(text);
        KeyPin pin;
        j.at("epoch").get_to(pin.epoch);
        j.at("seq").get_to(pin.seq);
        j.at("commitment").get_to(pin.commitment);
        j.at("members").get_to(pin.members);
        if (const auto past = j.find("past"); past != j.end()) {
            for (const auto& [epoch, commitment] : past->items()) pin.past[std::stoll(epoch)] = commitment.get<std::string>();
        }
        return pin;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void StoredKeyPins::save(const std::string& selfUserId, const std::string& conversationId, const KeyPin& pin) {
    Json j{{"epoch", pin.epoch}, {"seq", pin.seq}, {"commitment", pin.commitment}, {"members", pin.members}};
    Json past = Json::object();
    for (const auto& [epoch, commitment] : pin.past) past[std::to_string(epoch)] = commitment;
    j["past"] = std::move(past);
    write_(toLowerAscii(selfUserId), toLowerAscii(conversationId), j.dump());
}

bool MemoryContactKeyPins::keyChanged(const std::string& selfUserId, const std::string& contactUserId, const std::string& publicKeyB64) {
    if (publicKeyB64.empty()) return false;
    std::lock_guard lock(mutex_);
    return pins_.try_emplace(pinName(selfUserId, contactUserId), publicKeyB64).first->second != publicKeyB64;
}

void requireUnchangedKey(ContactKeyPins* contacts, const std::string& selfUserId, const std::string& contactUserId, const std::string& publicKeyB64) {
    if (contacts != nullptr && contacts->keyChanged(selfUserId, contactUserId, publicKeyB64)) {
        throw api::UserFacingError(KeyTrust::kContactKeyChanged);
    }
}

KeyTrust::KeyTrust(std::shared_ptr<KeyPins> pins, std::shared_ptr<ContactKeyPins> contacts)
    : pins_(std::move(pins)), contacts_(std::move(contacts)) {}

void KeyTrust::requireUnchangedKey(const std::string& selfUserId, const std::string& contactUserId, const std::string& publicKeyB64) {
    data::requireUnchangedKey(contacts_.get(), selfUserId, contactUserId, publicKeyB64);
}

bool KeyTrust::isPinned(const std::string& selfUserId, const std::string& conversationId) {
    std::lock_guard lock(mutex_);
    return pins_->load(selfUserId, conversationId).has_value();
}

std::optional<std::string> KeyTrust::checkDirectKey(const std::string& selfUserId, const std::string& conversationId, std::int64_t epoch,
                                                    const crypto::SecretBytes* key) {
    if (epoch != 0) return kDirectKeyChanged;
    if (key == nullptr) return kDirectKeyUnopened;
    const std::string commitment = commitmentOf(*key);
    if (commitment.empty()) return kDirectKeyUnopened;
    std::lock_guard lock(mutex_);
    const auto pin = pins_->load(selfUserId, conversationId);
    if (pin && pin->commitment != commitment) return kDirectKeyChanged;
    if (!pin) pins_->save(selfUserId, conversationId, KeyPin{0, 0, commitment, {}});
    return std::nullopt;
}

bool KeyTrust::matchesPin(const std::string& selfUserId, const std::string& conversationId, const crypto::SecretBytes& key) {
    std::lock_guard lock(mutex_);
    const auto pin = pins_->load(selfUserId, conversationId);
    return pin && pin->commitment == commitmentOf(key);
}

std::optional<std::string> KeyTrust::checkGroupKey(const GroupKeyCheck& check) {
    std::lock_guard lock(mutex_);
    const auto pin = pins_->load(check.selfUserId, check.conversationId);
    // Groups from before key notices have none, until a member's app publishes the first. Once this computer has
    // seen one, its absence means someone removed it.
    if (check.notice == nullptr || !check.notice->has_value()) {
        if (pin) return kGroupKeyUnverified;
        return std::nullopt;
    }
    const api::KeyNoticeDto& notice = **check.notice;
    if (check.key == nullptr) return kGroupKeyUnopened;
    if (notice.epoch != check.epoch) return kGroupKeyUnverified;
    if (pin && (notice.epoch < pin->epoch || (notice.epoch == pin->epoch && notice.seq < pin->seq))) return kGroupKeyUnverified;
    crypto::Bytes commitment;
    try {
        commitment = crypto::keyCommitment(*check.key);
    } catch (const crypto::CryptoError&) {
        return kGroupKeyUnverified;
    }
    const std::string commitmentB64 = base64Encode(commitment);
    const std::string issuer = toLowerAscii(notice.issuerId);
    const std::vector<std::string> members = lowered(notice.members);
    // Two notices never share a sequence number, so this one has to be the notice already seen.
    if (pin && notice.epoch == pin->epoch && notice.seq == pin->seq && !sameMembers(members, pin->members)) return kGroupKeyUnverified;
    // The same epoch has to mean the same key.
    if (pin && notice.epoch == pin->epoch && pin->commitment != commitmentB64) return kGroupKeyUnverified;
    // Someone outside the group can't start a new epoch. An unchanged key proves the issuer already holds it.
    if (pin && notice.epoch > pin->epoch && !contains(pin->members, issuer)) return kGroupKeyUnverified;
    if (!contains(members, issuer) || !contains(members, toLowerAscii(check.selfUserId))) return kGroupKeyUnverified;
    if (!noticeSignatureOk(check.conversationId, notice, *check.key, check.signingKey)) return kGroupKeyUnverified;
    std::map<std::int64_t, std::string> past;
    if (pin) {
        past = pin->past;
        if (notice.epoch > pin->epoch) past[pin->epoch] = pin->commitment;
    }
    pins_->save(check.selfUserId, check.conversationId, KeyPin{notice.epoch, notice.seq, commitmentB64, members, std::move(past)});
    return std::nullopt;
}

bool KeyTrust::noticeSignatureOk(const std::string& conversationId, const api::KeyNoticeDto& notice, const crypto::SecretBytes& key,
                                 const std::function<std::optional<crypto::Bytes>(const std::string&)>& signingKey) const {
    const std::string issuer = toLowerAscii(notice.issuerId);
    const auto issuerKey = signingKey ? signingKey(issuer) : std::nullopt;
    if (!issuerKey) return false;
    try {
        const auto input = crypto::keyNoticeInput(conversationId, issuer, notice.epoch, notice.seq, lowered(notice.members), crypto::keyCommitment(key));
        return crypto::verifyMessageSignature(base64Decode(notice.signature), input, *issuerKey);
    } catch (const std::exception&) {
        return false;  // Not base64, not a UUID, or a number out of range.
    }
}

bool KeyTrust::matchesVerifiedEpoch(const std::string& selfUserId, const std::string& conversationId, std::int64_t epoch, const crypto::SecretBytes& key) {
    std::lock_guard lock(mutex_);
    const auto pin = pins_->load(selfUserId, conversationId);
    if (!pin) return true;
    if (pin->epoch == epoch) return pin->commitment == commitmentOf(key);
    const auto past = pin->past.find(epoch);
    return past == pin->past.end() || past->second == commitmentOf(key);
}

std::vector<std::string> KeyTrust::membersToSign(const std::string& selfUserId, const std::string& conversationId, const std::vector<std::string>& listed,
                                                 const std::vector<std::string>& added, const std::optional<api::KeyNoticeDto>& latest) {
    const std::vector<std::string> current = lowered(listed);
    std::optional<std::vector<std::string>> verified;
    {
        std::lock_guard lock(mutex_);
        if (const auto pin = pins_->load(selfUserId, conversationId); pin && !pin->members.empty()) verified = pin->members;
    }
    if (!verified && latest) verified = lowered(latest->members);
    std::vector<std::string> out;
    const auto add = [&](const std::string& id) {
        if (!contains(out, id)) out.push_back(id);
    };
    for (const auto& id : verified ? *verified : current) {
        if (!verified || contains(current, id)) add(id);
    }
    for (const auto& id : lowered(added)) add(id);
    return out;
}

std::vector<std::string> KeyTrust::unconfirmedMembers(const std::string& selfUserId, const std::string& conversationId,
                                                      const std::vector<std::string>& participants) {
    std::lock_guard lock(mutex_);
    const auto pin = pins_->load(selfUserId, conversationId);
    std::vector<std::string> out;
    if (!pin || pin->members.empty()) return out;
    for (const auto& participant : participants) {
        if (!contains(pin->members, toLowerAscii(participant))) out.push_back(participant);
    }
    return out;
}

std::int64_t KeyTrust::nextSeq(const std::string& selfUserId, const std::string& conversationId, std::int64_t epoch,
                               const std::optional<api::KeyNoticeDto>& latest) {
    std::lock_guard lock(mutex_);
    const auto pin = pins_->load(selfUserId, conversationId);
    const std::int64_t pinned = pin && pin->epoch == epoch ? pin->seq : 0;
    const std::int64_t published = latest && latest->epoch == epoch ? latest->seq : 0;
    return std::max(pinned, published) + 1;
}

std::optional<KeyPin> KeyTrust::pin(const std::string& selfUserId, const std::string& conversationId) {
    std::lock_guard lock(mutex_);
    return pins_->load(selfUserId, conversationId);
}

}  // namespace redvault::data
