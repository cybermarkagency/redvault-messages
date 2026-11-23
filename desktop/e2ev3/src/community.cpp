// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/community.hpp"

#include <sodium.h>

#include <algorithm>
#include <set>

#include "batches.hpp"
#include "fs_util.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/api/community_api.hpp"
#include "redvault/e2ev3/chat.hpp"
#include "redvault/e2ev3/device.hpp"
#include "redvault/e2ev3/send_queue.hpp"
#include "redvault/util/text.hpp"

namespace redvault::e2ev3 {

namespace {

/// The most accounts a signed roster holds (crypto-core/src/roster.rs MAX_MEMBERS, the server's roster route).
constexpr std::size_t kMaxRosterMembers = 1100;
/// One page of membership events.
constexpr int kTaskPage = 200;

std::string text(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<std::string> strings(const Json& j) {
    std::vector<std::string> out;
    if (!j.is_array()) return out;
    for (const Json& v : j) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

std::vector<std::string> sortedUnique(std::vector<std::string> v) {
    std::erase_if(v, [](const std::string& s) { return s.empty(); });
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

bool has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

/// The accounts a refusal names in details.userIds (roster_member_not_entitled, roster_member_not_staff).
std::vector<std::string> namedAccounts(const api::ApiError& error) {
    const Json details = Json::parse(error.details(), nullptr, false);
    return details.is_object() ? strings(details.value("userIds", Json::array())) : std::vector<std::string>{};
}

/// The community's roster chain from the server, verified here; the latest verify answer, or null when there is none yet.
Json latestRoster(V3Device& device, const std::string& conversationId) {
    const Json chain = device.server().rosters(conversationId, 0);
    if (!chain.is_array() || chain.empty()) return nullptr;
    const Json fields = {{"groupId", conversationId}, {"chain", chain}};
    Json answer = device.op("rosterVerify", fields);
    if (!answer.value("ok", false) && answer.value("error", std::string()) == "unknown_issuer") {
        // A roster signed by someone whose list this device hasn't checked yet (a community's owner or staff, who
        // may never have written here): verify each issuer's list, then the chain once more.
        std::set<std::string> issuers;
        for (const Json& r : chain) {
            if (r.is_object() && r.contains("issuer") && r.at("issuer").is_string()) issuers.insert(r.at("issuer").get<std::string>());
        }
        for (const std::string& issuer : issuers) {
            if (issuer != device.userId()) verifiedDevices(device, issuer, conversationId);
        }
        answer = device.op("rosterVerify", fields);
    }
    return requireOk(std::move(answer), "rosterVerify");
}

std::vector<std::string> membersOf(const Json& verified) {
    return verified.is_null() ? std::vector<std::string>{} : strings(verified.value("members", Json::array()));
}

std::int64_t epochOf(const Json& verified) {
    return verified.is_object() ? verified.value("epoch", std::int64_t{0}) : 0;
}

/**
 * Every listed device of the conversation's entitled accounts that the caller may see (GET .../device-lists: a
 * channel reader sees only the staff and themself), from lists this device verified. A list that doesn't verify is
 * left out (it gets nothing) rather than stopping a message to a thousand people.
 */
std::vector<Json> communityDevices(V3Device& device, const std::string& conversationId) {
    std::vector<Json> out;
    const Json view = device.server().conversationDeviceLists(conversationId);
    for (const Json& member : view.value("members", Json::array())) {
        const auto list = member.find("deviceList");
        if (list == member.end() || list->is_null()) continue;
        const Json checked = device.op("verifyDeviceList", Json{{"deviceList", *list}});
        if (!checked.value("ok", false)) continue;
        for (const Json& d : checked.value("devices", Json::array())) out.push_back(d);
    }
    return out;
}

/**
 * Shares this device's sender key with every device of `accounts` (but this one), then sends `plaintext` (if any)
 * under it, both in batches of at most 100 devices. A channel shares with its readers ("audience": "recipients").
 * Accounts the server says aren't entitled are dropped (into `dropped`) and the batch tried once more. Returns how
 * many devices the message went to (or the key, without a message).
 */
int shareAndSend(V3Device& device, const CommunityInfo& community, const std::vector<std::string>& accounts, const std::string* plaintext,
                 std::set<std::string>* dropped) {
    const std::string& conversationId = community.conversationId;
    std::vector<Json> others;
    for (const Json& d : communityDevices(device, conversationId)) {
        if (text(d, "deviceId") == device.deviceId() || !has(accounts, text(d, "userId"))) continue;
        others.push_back(d);
    }
    Json recipients = Json::array();
    for (const Json& d : others) recipients.push_back(Json{{"identity", d}});
    Json request = {{"groupId", conversationId}, {"recipients", recipients}};
    if (community.channel()) request["audience"] = "recipients";
    const Json shared = device.must("groupSignalShare", request);
    Json results = shared.at("results");
    // Devices without a Signal session yet: claim their bundles (100 at a time) and share once more.
    Json ask = Json::array();
    std::vector<std::size_t> need;
    for (std::size_t i = 0; i < others.size(); ++i) {
        if (results.at(i).value("error", std::string()) == "no_signal_session") {
            need.push_back(i);
            ask.push_back({{"userId", others[i].at("userId")}, {"deviceId", others[i].at("deviceId")}});
        }
    }
    if (!need.empty()) {
        const Json claimed = detail::claimInBatches(ask, [&](const Json& batch) {
            try {
                return device.server().signalClaim(conversationId, batch);
            } catch (const api::ApiError& error) {
                // One account in the batch isn't entitled any more (404): the server refuses the whole claim. Claim
                // each device alone, so only that account goes without (it gets nothing, never an Olm copy).
                if (error.status() != 404 || batch.size() == 1) {
                    if (error.status() != 404) throw;
                    return Json::array({Json{{"certificate", nullptr}, {"bundle", nullptr}}});
                }
                Json each = Json::array();
                for (const Json& one : batch) {
                    try {
                        each.push_back(device.server().signalClaim(conversationId, Json::array({one})).at(0));
                    } catch (const api::ApiError& inner) {
                        if (inner.status() != 404) throw;
                        each.push_back(Json{{"certificate", nullptr}, {"bundle", nullptr}});
                    }
                }
                return each;
            }
        });
        Json retry = Json::array();
        for (std::size_t k = 0; k < need.size(); ++k) {
            const Json& c = claimed.at(k);
            retry.push_back(Json{{"identity", others[need[k]]},
                                 {"signalBundle", Json{{"certificate", c.value("certificate", Json(nullptr))}, {"bundle", c.value("bundle", Json(nullptr))}}}});
        }
        Json again = {{"groupId", conversationId}, {"recipients", retry}};
        if (community.channel()) again["audience"] = "recipients";
        const Json answer = device.must("groupSignalShare", again);
        for (std::size_t k = 0; k < need.size(); ++k) results[need[k]] = answer.at("results").at(k);
    }
    Json keyMessages = Json::array();
    for (std::size_t i = 0; i < others.size(); ++i) {
        const Json& r = results.at(i);
        if (r.contains("message")) {
            keyMessages.push_back({{"userId", others[i].at("userId")}, {"deviceId", others[i].at("deviceId")}, {"body", r.at("message").dump()}});
        }
    }
    std::set<std::string> gone;
    int keys = 0;
    if (!keyMessages.empty()) {
        keys = detail::sendInBatches(
            keyMessages, [&](const Json& b) { device.server().sendMessages(device.deviceId(), conversationId, b); }, true, &gone);
    }
    int delivered = keys;
    if (plaintext) {
        const Json sealed = device.must("groupSignalEncrypt", Json{{"groupId", conversationId}, {"plaintext", *plaintext}});
        const std::string body = sealed.at("message").dump();
        Json fanout = Json::array();
        for (const Json& d : others) {
            if (!gone.count(text(d, "userId"))) fanout.push_back({{"userId", d.at("userId")}, {"deviceId", d.at("deviceId")}});
        }
        // Every listed device of each account still addressed (an account the server refused is dropped whole):
        // marked allDevices (docs/signal-integration/13-every-device-and-session-reset.md), split by account.
        delivered = detail::sendInBatches(
            fanout, [&](const Json& b) { device.server().sendGroupMessage(device.deviceId(), conversationId, body, b, true); }, gone.empty(),
            &gone, true);
    }
    if (dropped) dropped->insert(gone.begin(), gone.end());
    return delivered;
}

/**
 * A channel's readers as this staff device sends to them: the server's recipient list, the staff, and this account.
 * When an account this device last shared its key with isn't on it any more, the staff roster is issued again at
 * the next epoch first, so the next sender key is one the leaver never gets.
 */
std::vector<std::string> channelAudience(V3Device& device, const CommunityInfo& community, const std::vector<std::string>& staff,
                                         bool mayRotate = true) {
    Communities& communities = *device.backend().communities;
    std::vector<std::string> accounts = strings(communities.directory().recipients(community.communityId).value("userIds", Json::array()));
    accounts.insert(accounts.end(), staff.begin(), staff.end());
    accounts.push_back(device.userId());
    accounts = sortedUnique(std::move(accounts));
    bool left = false;
    for (const std::string& before : communities.audience(community.conversationId)) left = left || !has(accounts, before);
    if (left && mayRotate) issueCommunityRoster(device, community, staff, true);
    return accounts;
}

/// After a channel send or share: who this device's sender key went to (the accounts the server refused left out).
void rememberAudience(V3Device& device, const CommunityInfo& community, std::vector<std::string> accounts, const std::set<std::string>& dropped) {
    std::erase_if(accounts, [&](const std::string& a) { return dropped.count(a) > 0; });
    device.backend().communities->setAudience(community.conversationId, std::move(accounts));
}

/// Owner/staff: the roster this community should have now (group: the entitled accounts; channel: the staff).
std::int64_t ensureRoster(V3Device& device, const CommunityInfo& community) {
    CommunityDirectory& directory = device.backend().communities->directory();
    std::vector<std::string> members = community.channel()
                                           ? directory.staff(community.communityId)
                                           : strings(directory.recipients(community.communityId).value("userIds", Json::array()));
    return issueCommunityRoster(device, community, std::move(members));
}

// --- The sealed state file ---

std::string seal(const std::string& plain, const crypto::Bytes& key) {
    const std::size_t nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    std::string blob(nonceLen + plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    randombytes_buf(blob.data(), nonceLen);
    unsigned long long cipherLen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(blob.data()) + nonceLen, &cipherLen,
                                               reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), nullptr, 0, nullptr,
                                               reinterpret_cast<const unsigned char*>(blob.data()), key.data());
    blob.resize(nonceLen + cipherLen);
    return blob;
}

std::optional<std::string> unseal(const std::string& blob, const crypto::Bytes& key) {
    const std::size_t nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (blob.size() <= nonceLen + crypto_aead_xchacha20poly1305_ietf_ABYTES) return std::nullopt;
    const auto* data = reinterpret_cast<const unsigned char*>(blob.data());
    std::string plain(blob.size() - nonceLen, '\0');
    unsigned long long plainLen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()), &plainLen, nullptr, data + nonceLen,
                                                   blob.size() - nonceLen, nullptr, 0, data, key.data()) != 0) {
        return std::nullopt;
    }
    plain.resize(plainLen);
    return plain;
}

std::int64_t resolveNow(std::int64_t nowMs) {
    return nowMs > 0 ? nowMs : util::nowMillis();
}

}  // namespace

// --- HttpCommunityDirectory ---

Json HttpCommunityDirectory::membershipTasks() {
    return api::community::membershipTasks(*api_);
}
Json HttpCommunityDirectory::membershipTasksFor(const std::string& communityId, std::int64_t after, int limit) {
    return api::community::membershipTasksFor(*api_, communityId, after, limit);
}
Json HttpCommunityDirectory::admit(const std::string& communityId, const std::string& userId) {
    return api::community::admit(*api_, communityId, userId);
}
Json HttpCommunityDirectory::recipients(const std::string& communityId) {
    return api::community::recipients(*api_, communityId);
}
std::vector<std::string> HttpCommunityDirectory::staff(const std::string& communityId) {
    return api::community::staffAccounts(*api_, communityId);
}

// --- Communities ---

Communities::Communities(std::shared_ptr<CommunityDirectory> directory, std::filesystem::path stateFile, crypto::Bytes key)
    : directory_(std::move(directory)), file_(std::move(stateFile)), key_(std::move(key)) {
    crypto::ensureSodium();
    if (!file_.empty() && key_.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
        throw std::invalid_argument("Communities needs a 32-byte key for its file");
    }
    load();
}

void Communities::attachFile(std::filesystem::path stateFile, crypto::Bytes key) {
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) throw std::invalid_argument("Communities needs a 32-byte key for its file");
    std::lock_guard lock(mutex_);
    file_ = std::move(stateFile);
    key_ = std::move(key);
    cursors_.clear();
    audiences_.clear();
    load();
}

void Communities::load() {
    if (file_.empty()) return;
    const auto blob = fsutil::readAll(file_);
    if (!blob) return;
    const auto plain = unseal(*blob, key_);
    if (!plain) return;  // another device's key: start again (the cursors are only a shortcut)
    const Json j = Json::parse(*plain, nullptr, false);
    if (!j.is_object()) return;
    const Json cursors = j.value("cursors", Json::object());
    for (auto it = cursors.begin(); it != cursors.end(); ++it) {
        if (it.value().is_number_integer()) cursors_[it.key()] = it.value().get<std::int64_t>();
    }
    const Json audiences = j.value("audiences", Json::object());
    for (auto it = audiences.begin(); it != audiences.end(); ++it) audiences_[it.key()] = strings(it.value());
}

void Communities::save() const {
    if (file_.empty()) return;
    const std::string plain = Json{{"cursors", cursors_}, {"audiences", audiences_}}.dump();
    std::error_code ignored;
    std::filesystem::create_directories(file_.parent_path(), ignored);
    fsutil::writeAtomic(file_, seal(plain, key_));
}

void Communities::setAll(const std::vector<CommunityInfo>& communities) {
    std::lock_guard lock(mutex_);
    byConversation_.clear();
    for (const CommunityInfo& c : communities) byConversation_[c.conversationId] = c;
}

void Communities::set(const CommunityInfo& community) {
    std::lock_guard lock(mutex_);
    byConversation_[community.conversationId] = community;
}

std::optional<CommunityInfo> Communities::find(const std::string& conversationId) const {
    std::lock_guard lock(mutex_);
    const auto it = byConversation_.find(conversationId);
    if (it == byConversation_.end()) return std::nullopt;
    return it->second;
}

std::optional<CommunityInfo> Communities::byCommunity(const std::string& communityId) const {
    std::lock_guard lock(mutex_);
    for (const auto& [id, c] : byConversation_) {
        if (c.communityId == communityId) return c;
    }
    return std::nullopt;
}

std::vector<CommunityInfo> Communities::all() const {
    std::lock_guard lock(mutex_);
    std::vector<CommunityInfo> out;
    for (const auto& [id, c] : byConversation_) out.push_back(c);
    return out;
}

std::int64_t Communities::cursor(const std::string& communityId) const {
    std::lock_guard lock(mutex_);
    const auto it = cursors_.find(communityId);
    return it == cursors_.end() ? 0 : it->second;
}

void Communities::setCursor(const std::string& communityId, std::int64_t seq) {
    std::lock_guard lock(mutex_);
    cursors_[communityId] = seq;
    save();
}

std::vector<std::string> Communities::audience(const std::string& conversationId) const {
    std::lock_guard lock(mutex_);
    const auto it = audiences_.find(conversationId);
    return it == audiences_.end() ? std::vector<std::string>{} : it->second;
}

void Communities::setAudience(const std::string& conversationId, std::vector<std::string> accounts) {
    std::lock_guard lock(mutex_);
    audiences_[conversationId] = sortedUnique(std::move(accounts));
    save();
}

// --- Sending ---

std::optional<CommunityInfo> communityOf(const V3Device& device, const std::string& conversationId) {
    const auto& communities = device.backend().communities;
    if (!communities || conversationId.empty()) return std::nullopt;
    return communities->find(conversationId);
}

std::int64_t issueCommunityRoster(V3Device& device, const CommunityInfo& community, std::vector<std::string> members, bool rotate) {
    const std::string& conversationId = community.conversationId;
    members.push_back(device.userId());
    members = sortedUnique(std::move(members));
    std::int64_t startedAt = -1;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (members.size() > kMaxRosterMembers) {
            throw EngineError("community_too_large", "A community's roster holds at most 1,100 accounts.");
        }
        const Json latest = latestRoster(device, conversationId);
        if (startedAt < 0) startedAt = epochOf(latest);
        // Another staff device rotated while this one tried: that's the rotation asked for.
        if (rotate && epochOf(latest) > startedAt) return 0;
        Json made;
        if (latest.is_null()) {
            made = device.peek("rosterCreate", Json{{"groupId", conversationId}, {"members", members}});
        } else {
            const std::vector<std::string> current = membersOf(latest);
            if (current == members && !rotate) return 0;
            if (!has(current, device.userId())) {
                throw EngineError("not_in_roster", "Another owner or staff member's app has to add you to this community's roster first.");
            }
            made = device.peek("rosterUpdate", Json{{"groupId", conversationId}, {"members", members}});
        }
        try {
            device.server().postRoster(conversationId, made.at("roster"));
        } catch (const api::ApiError& error) {
            if (error.code() == "stale_roster") continue;  // another staff device's roster came first: build on it
            if (error.code() == "roster_member_not_entitled" || error.code() == "roster_member_not_staff") {
                const auto refused = namedAccounts(error);
                const auto before = members.size();
                std::erase_if(members, [&](const std::string& m) { return m != device.userId() && has(refused, m); });
                if (members.size() == before) throw;
                continue;
            }
            throw;
        }
        // Taken: this device adopts it from the server's chain, like any other member.
        latestRoster(device, conversationId);
        return made.at("roster").value("epoch", std::int64_t{0});
    }
    throw EngineError("roster_busy", "The community's roster kept changing. Try again in a moment.");
}

int sendToCommunity(V3Device& device, const CommunityInfo& community, const std::string& plaintext, bool isOp) {
    if (!engineHasSignal()) throw EngineError("signal_required", "Communities need Signal sessions, which this build doesn't include.");
    const std::string& conversationId = community.conversationId;
    if (!isOp && community.channel() && !community.staff) throw EngineError("cannot_post", "Only the owner and staff post in this channel.");
    if (!isOp && !community.canPost) throw EngineError("cannot_post", "You can't post in this community.");
    Json roster = latestRoster(device, conversationId);
    if (community.staff && (roster.is_null() || !has(membersOf(roster), device.userId()))) {
        ensureRoster(device, community);
        roster = latestRoster(device, conversationId);
    }
    if (roster.is_null()) {
        throw EngineError("community_not_ready", "This community isn't ready for messages yet. The owner's app sets it up; try again in a minute.");
    }
    const std::vector<std::string> members = membersOf(roster);
    if (!has(members, device.userId())) {
        if (community.channel() && isOp) {
            // A reader's reaction or vote: pairwise to the staff's devices (and this account's other devices), never a fan-out.
            std::vector<Json> recipients;
            for (const Json& d : communityDevices(device, conversationId)) {
                const std::string user = text(d, "userId");
                if (text(d, "deviceId") != device.deviceId() && (has(members, user) || user == device.userId())) recipients.push_back(d);
            }
            const SendScope* outer = SendScope::current();
            std::optional<SendScope> scope;
            if (outer && !outer->recordId.empty()) scope.emplace(outer->recordId, "messages");  // this request carries the op
            FanOut options;
            options.allDevices = true;  // the staff's and this account's listed devices: marked when none is unreachable
            return sendToDevices(device, conversationId, recipients, plaintext, nullptr, options);
        }
        if (community.channel()) throw EngineError("cannot_post", "Only the owner and staff post in this channel.");
        throw EngineError("not_in_roster", "You can post once the owner's or a staff member's app has added you. Try again in a minute.");
    }
    std::set<std::string> dropped;
    if (community.channel()) {
        const std::vector<std::string> audience = channelAudience(device, community, members);
        const int delivered = shareAndSend(device, community, audience, &plaintext, &dropped);
        rememberAudience(device, community, audience, dropped);
        return delivered;
    }
    return shareAndSend(device, community, members, &plaintext, &dropped);
}

namespace {

int shareChannelKeyAfter(V3Device& device, const CommunityInfo& community, bool rotated) {
    if (!community.channel() || !device.backend().communities) return 0;
    const Json roster = latestRoster(device, community.conversationId);
    const std::vector<std::string> staff = membersOf(roster);
    if (!has(staff, device.userId())) return 0;
    const std::vector<std::string> audience = channelAudience(device, community, staff, !rotated);
    std::set<std::string> dropped;
    const int shared = shareAndSend(device, community, audience, nullptr, &dropped);
    rememberAudience(device, community, audience, dropped);
    return shared;
}

}  // namespace

int shareChannelKey(V3Device& device, const CommunityInfo& community) {
    return shareChannelKeyAfter(device, community, false);
}

// --- Membership tasks ---

MembershipRound runMembershipTasks(V3Device& device) {
    MembershipRound round;
    const auto communities = device.backend().communities;
    if (!communities) return round;
    CommunityDirectory& directory = communities->directory();
    const Json rows = directory.membershipTasks();
    if (!rows.is_array()) return round;
    for (const Json& row : rows) {
        CommunityInfo info;
        info.communityId = text(row, "communityId");
        info.conversationId = text(row, "conversationId");
        if (info.communityId.empty() || info.conversationId.empty()) continue;
        if (const auto known = communities->find(info.conversationId)) info = *known;
        info.kind = text(row, "kind") == "channel" ? "channel" : "group";
        info.staff = true;  // the server lists only communities where this account is active staff
        communities->set(info);
        const std::int64_t cursor = communities->cursor(info.communityId);
        const std::int64_t latestSeq = row.value("latestSeq", std::int64_t{0});
        const int waiting = row.value("waiting", 0);
        if (latestSeq == cursor && waiting == 0) continue;
        ++round.communities;
        try {
            std::int64_t after = cursor;
            bool leaves = false;
            bool roles = false;
            std::vector<std::string> waitingList;
            std::vector<std::string> lapsed;
            const auto readEvents = [&] {
                for (int page = 0; page < 1000; ++page) {
                    const Json tasks = directory.membershipTasksFor(info.communityId, after, kTaskPage);
                    for (const Json& event : tasks.value("events", Json::array())) {
                        const std::string kind = text(event, "kind");
                        leaves = leaves || kind == "leave";
                        roles = roles || kind == "role";
                    }
                    after = tasks.value("cursor", after);
                    waitingList = strings(tasks.value("waiting", Json::array()));
                    lapsed = strings(tasks.value("lapsed", Json::array()));
                    if (!tasks.value("hasMore", false)) break;
                }
            };
            readEvents();
            int admitted = 0;
            for (const std::string& userId : waitingList) {
                try {
                    directory.admit(info.communityId, userId);
                    ++admitted;
                } catch (const api::ApiError& error) {
                    // Someone else's app admitted them first, their payment lapsed, or the community is full: next one.
                    if (error.status() != 409) throw;
                }
            }
            round.admitted += admitted;
            // The joins just made are events too: read them now, so the next round doesn't do this again.
            if (admitted > 0) readEvents();
            if (!info.channel()) {
                std::vector<std::string> target = strings(directory.recipients(info.communityId).value("userIds", Json::array()));
                std::erase_if(target, [&](const std::string& u) { return has(lapsed, u); });
                if (issueCommunityRoster(device, info, std::move(target)) > 0) ++round.rosters;
            } else {
                const std::vector<std::string> staff = sortedUnique(directory.staff(info.communityId));
                const Json roster = latestRoster(device, info.conversationId);
                std::vector<std::string> rosterStaff = staff;
                rosterStaff.push_back(device.userId());
                rosterStaff = sortedUnique(std::move(rosterStaff));
                if (roster.is_null() || roles || membersOf(roster) != rosterStaff) {
                    if (issueCommunityRoster(device, info, staff) > 0) ++round.rosters;
                }
                bool rotated = false;
                if (leaves) {
                    if (issueCommunityRoster(device, info, membersOf(latestRoster(device, info.conversationId)), true) > 0) {
                        ++round.rosters;
                        ++round.rotations;
                    }
                    rotated = true;  // done (here or by another staff device): sharing below doesn't rotate again
                }
                round.shared += shareChannelKeyAfter(device, info, rotated);
            }
            communities->setCursor(info.communityId, after);
        } catch (const api::ApiError& error) {
            round.errors.push_back(info.communityId + ": " + (error.code().empty() ? "error" : error.code()));
        } catch (const EngineError& error) {
            round.errors.push_back(info.communityId + ": " + error.code());
        }
    }
    return round;
}

// --- The notice ---

void noteCommunityJoined(ChatHistory& history, const std::string& conversationId, std::int64_t nowMs) {
    history.markJoined(conversationId, resolveNow(nowMs));
}

std::string communityNotice(const ChatHistory& history, const std::string& conversationId) {
    return history.joinedAt(conversationId) ? kBeforeYouJoined : "";
}

}  // namespace redvault::e2ev3
