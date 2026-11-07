#include "redvault/data/community_keys.hpp"

#include <utility>

#include "redvault/api/community_api.hpp"

namespace redvault::data {

namespace community = crypto::community;
namespace community_api = api::community;

CommunityKeyStore::CommunityKeyStore(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const SelfIdentity> identity)
    : api_(std::move(api)), identity_(std::move(identity)) {}

CommunityKeyStore::Ring CommunityKeyStore::ring(const std::string& conversationId) const {
    std::lock_guard lock(mutex_);
    const auto it = rings_.find(conversationId);
    return it == rings_.end() ? nullptr : it->second;
}

CommunityKeyStore::Ring CommunityKeyStore::store(community::KeyRing ring, const std::string& conversationId) {
    if (ring.conversationId.empty()) ring.conversationId = conversationId;
    auto shared = std::make_shared<const community::KeyRing>(std::move(ring));
    std::lock_guard lock(mutex_);
    rings_[shared->conversationId] = shared;
    return shared;
}

CommunityKeyStore::Ring CommunityKeyStore::load(const std::string& communityId, const std::string& conversationId) {
    const community::KeyState state = community_api::keys(*api_, communityId);
    return store(community::buildKeyRing(communityId, state, identity_->publicKey, identity_->privateKey), conversationId);
}

CommunityKeyStore::Ring CommunityKeyStore::ensure(const std::string& communityId, const std::string& conversationId,
                                                  std::int64_t keyEpoch, bool rekeyRequired) {
    if (auto known = ring(conversationId);
        known && known->currentEpoch == keyEpoch && known->rekeyRequired == rekeyRequired && known->key(keyEpoch) != nullptr) {
        return known;
    }
    return load(communityId, conversationId);
}

CommunityKeyStore::Ring CommunityKeyStore::rotate(const std::string& communityId, const std::string& conversationId) {
    const Ring current = load(communityId, conversationId);
    const community_api::Roster roster = community_api::roster(*api_, communityId);
    // Someone else already rotated: what's needed is their key, not another one.
    if (!roster.rekeyRequired && roster.keyEpoch != current->currentEpoch) return load(communityId, conversationId);
    const community::Rotation prepared = community::prepareRotation(*current, roster.keyEpoch, roster.members);
    community_api::rotate(*api_, communityId, prepared.fromEpoch, prepared.keys);
    return load(communityId, conversationId);
}

int CommunityKeyStore::enrollWaiting(const std::string& communityId, const std::string& conversationId) {
    const Ring current = load(communityId, conversationId);
    if (current->rekeyRequired || !current->hasCurrentKey()) return 0;
    const auto pending = community_api::pendingEnrollments(*api_, communityId);
    int enrolled = 0;
    for (const auto& enrollment : community::prepareEnrollments(*current, pending)) {
        try {
            community_api::enroll(*api_, communityId, enrollment);
            ++enrolled;
        } catch (const api::ApiError& e) {
            // Someone else's app got there first, or the epoch just moved: the next round sorts it out.
            if (e.status() != 409) throw;
        }
    }
    return enrolled;
}

CommunityKeyStore::Upkeep CommunityKeyStore::runMaintenance() {
    if (running_.exchange(true)) return {};
    struct Reset {
        std::atomic<bool>& flag;
        ~Reset() { flag = false; }
    } reset{running_};

    Upkeep done;
    for (const auto& task : community_api::keyTasks(*api_)) {
        try {
            if (task.rekeyRequired) {
                // Only members who can post may rotate; the others wait for one of them.
                if (!task.canPost) continue;
                rotate(task.communityId, task.conversationId);
                ++done.rotated;
            }
            if (task.pendingEnrollments > 0) done.enrolled += enrollWaiting(task.communityId, task.conversationId);
        } catch (const api::ApiError& e) {
            if (e.status() == 401) throw;  // the session is over: the caller signs out
            // 409: another member's app raced this one. Anything else: tried again next round.
        } catch (const std::exception&) {
            // A key that won't open or a roster key that won't seal: tried again next round.
        }
    }
    return done;
}

void CommunityKeyStore::forget() {
    std::lock_guard lock(mutex_);
    rings_.clear();
}

}  // namespace redvault::data
