// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/stories.hpp"

#include <sodium.h>

#include <algorithm>
#include <set>

#include "fs_util.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/e2ev3/send_queue.hpp"
#include "redvault/util/text.hpp"

namespace redvault::e2ev3 {

namespace {

std::string padded(std::string json) {
    constexpr std::size_t kBucket = 256;
    json.append((json.size() / kBucket + 1) * kBucket - json.size(), ' ');
    return json;
}

std::string stringAt(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::int64_t intAt(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number_integer() ? it->get<std::int64_t>() : 0;
}

/// What a story envelope must say to be sent: exactly what decodeEnvelope takes.
void checkDecodes(const std::string& plaintext, const std::string& storyId, DecodedEnvelope::Kind kind) {
    const DecodedEnvelope d = decodeEnvelope(plaintext, storyId, util::nowMillis());
    if (d.kind != kind) throw EngineError("bad_story_message", "That story message wouldn't be accepted.");
}

}  // namespace

// --- Envelopes ---

std::string encodeStoryKey(const std::string& storyId, const crypto::SecretBytes& key, std::int64_t expiresAt, const std::string& id,
                           std::int64_t ts) {
    if (key.size() != crypto::stories::kKeyBytes) throw EngineError("bad_story_key", "A story key is 32 bytes.");
    const Json j{{"v", 1},       {"t", "storykey"},  {"id", id},
                 {"c", storyId}, {"ts", ts},         {"storyId", storyId},
                 {"key", crypto::base64Encode(key.view())}, {"expiresAt", expiresAt}};
    std::string out = padded(j.dump());
    checkDecodes(out, storyId, DecodedEnvelope::Kind::StoryKey);
    return out;
}

std::string encodeStoryAnswer(const std::string& storyId, const std::string& stickerId, std::int64_t rev, bool final, const Json& payload,
                              const std::string& id, std::int64_t ts) {
    const Json j{{"v", 1},           {"t", "storyanswer"}, {"id", id},       {"c", storyId}, {"ts", ts},
                 {"storyId", storyId}, {"stickerId", stickerId}, {"rev", rev}, {"final", final}, {"payload", payload}};
    std::string out = padded(j.dump());
    checkDecodes(out, storyId, DecodedEnvelope::Kind::StoryAnswer);
    return out;
}

// --- The store ---

StoryStore::StoryStore(std::filesystem::path file, crypto::Bytes key) : file_(std::move(file)), key_(std::move(key)) {
    crypto::ensureSodium();
    if (key_.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) throw std::invalid_argument("StoryStore needs a 32-byte key");
    load();
}

void StoryStore::setClock(std::function<std::int64_t()> clock) {
    std::lock_guard lock(mutex_);
    clock_ = std::move(clock);
}

std::int64_t StoryStore::now() const {
    return clock_ ? clock_() : util::nowMillis();
}

void StoryStore::load() {
    const auto blob = fsutil::readAll(file_);
    const std::size_t nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (!blob || blob->size() <= nonceLen) return;
    const auto* data = reinterpret_cast<const unsigned char*>(blob->data());
    std::string plain(blob->size() - nonceLen, '\0');
    unsigned long long plainLen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()), &plainLen, nullptr, data + nonceLen,
                                                   blob->size() - nonceLen, nullptr, 0, data, key_.data()) != 0) {
        return;  // another device's key (the device was replaced): nothing in it is this device's
    }
    plain.resize(plainLen);
    const Json j = Json::parse(plain, nullptr, false);
    sodium_memzero(plain.data(), plain.size());
    if (!j.is_object()) return;
    for (const Json& k : j.value("keys", Json::array())) {
        if (!k.is_object()) continue;
        try {
            crypto::Bytes raw = crypto::base64Decode(stringAt(k, "key"));
            if (raw.size() != crypto::stories::kKeyBytes) continue;
            Key key{stringAt(k, "storyId"), stringAt(k, "authorId"), std::make_shared<const crypto::SecretBytes>(crypto::SecretBytes::copyOf(raw)),
                    intAt(k, "expiresAt"), intAt(k, "arrivedAt"), k.value("own", false)};
            sodium_memzero(raw.data(), raw.size());
            keys_.push_back(std::move(key));
        } catch (const std::exception&) {
        }
    }
    for (const Json& a : j.value("answers", Json::array())) {
        if (!a.is_object()) continue;
        answers_.push_back(Answer{stringAt(a, "storyId"), stringAt(a, "stickerId"), stringAt(a, "userId"), intAt(a, "rev"), a.value("final", false),
                                  stringAt(a, "payload"), intAt(a, "at")});
    }
    for (const Json& d : j.value("deliveries", Json::array())) {
        if (!d.is_object()) continue;
        Delivery out;
        out.id = stringAt(d, "id");
        out.storyId = stringAt(d, "storyId");
        out.kind = stringAt(d, "kind");
        for (const Json& u : d.value("userIds", Json::array())) {
            if (u.is_string()) out.userIds.push_back(u.get<std::string>());
        }
        out.plaintext = stringAt(d, "plaintext");
        out.until = intAt(d, "until");
        for (const Json& u : d.value("done", Json::array())) {
            if (u.is_string()) out.done.push_back(u.get<std::string>());
        }
        out.attempts = static_cast<int>(intAt(d, "attempts"));
        deliveries_.push_back(std::move(out));
    }
}

void StoryStore::save() const {
    if (keys_.empty() && answers_.empty() && deliveries_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(file_, ignored);
        return;
    }
    Json keys = Json::array();
    for (const Key& k : keys_) {
        keys.push_back(Json{{"storyId", k.storyId}, {"authorId", k.authorId}, {"key", crypto::base64Encode(k.key->view())},
                            {"expiresAt", k.expiresAt}, {"arrivedAt", k.arrivedAt}, {"own", k.own}});
    }
    Json answers = Json::array();
    for (const Answer& a : answers_) {
        answers.push_back(Json{{"storyId", a.storyId}, {"stickerId", a.stickerId}, {"userId", a.userId}, {"rev", a.rev}, {"final", a.final},
                               {"payload", a.payload}, {"at", a.at}});
    }
    Json deliveries = Json::array();
    for (const Delivery& d : deliveries_) {
        deliveries.push_back(Json{{"id", d.id}, {"storyId", d.storyId}, {"kind", d.kind}, {"userIds", d.userIds}, {"plaintext", d.plaintext},
                                  {"until", d.until}, {"done", d.done}, {"attempts", d.attempts}});
    }
    std::string plain = Json{{"keys", keys}, {"answers", answers}, {"deliveries", deliveries}}.dump();
    const std::size_t nonceLen = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    std::string blob(nonceLen + plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    randombytes_buf(blob.data(), nonceLen);
    unsigned long long cipherLen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(blob.data()) + nonceLen, &cipherLen,
                                               reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), nullptr, 0, nullptr,
                                               reinterpret_cast<const unsigned char*>(blob.data()), key_.data());
    sodium_memzero(plain.data(), plain.size());
    blob.resize(nonceLen + cipherLen);
    std::error_code ignored;
    std::filesystem::create_directories(file_.parent_path(), ignored);
    fsutil::writeAtomic(file_, blob);
}

bool StoryStore::putKey(const std::string& storyId, const std::string& authorId, const crypto::SecretBytes& key, std::int64_t expiresAt, bool own) {
    std::lock_guard lock(mutex_);
    if (key.size() != crypto::stories::kKeyBytes || storyId.empty() || authorId.empty()) return false;
    const std::int64_t at = now();
    for (const Key& k : keys_) {
        if (k.storyId == storyId && k.authorId == authorId) return false;
    }
    keys_.push_back(Key{storyId, authorId, std::make_shared<const crypto::SecretBytes>(crypto::SecretBytes::copyOf(key.view())), expiresAt, at, own});
    save();
    return true;
}

std::optional<StoryStore::Key> StoryStore::key(const std::string& storyId, const std::string& authorId) const {
    std::lock_guard lock(mutex_);
    for (const Key& k : keys_) {
        if (k.storyId == storyId && k.authorId == authorId) return k;
    }
    return std::nullopt;
}

std::vector<StoryStore::Key> StoryStore::keys() const {
    std::lock_guard lock(mutex_);
    return keys_;
}

void StoryStore::forgetLocked(const std::string& storyId) {
    std::erase_if(keys_, [&](const Key& k) { return k.storyId == storyId; });
    std::erase_if(answers_, [&](const Answer& a) { return a.storyId == storyId; });
    std::erase_if(deliveries_, [&](const Delivery& d) { return d.storyId == storyId; });
}

void StoryStore::forget(const std::string& storyId) {
    std::lock_guard lock(mutex_);
    const auto before = keys_.size() + answers_.size() + deliveries_.size();
    forgetLocked(storyId);
    if (keys_.size() + answers_.size() + deliveries_.size() != before) save();
}

int StoryStore::dropStaleDeliveries() {
    std::lock_guard lock(mutex_);
    const std::int64_t at = now();
    const auto before = deliveries_.size();
    std::erase_if(deliveries_, [&](const Delivery& d) { return d.until <= at; });
    if (deliveries_.size() == before) return 0;
    save();
    return static_cast<int>(before - deliveries_.size());
}

bool StoryStore::putAnswer(const Answer& answer) {
    std::lock_guard lock(mutex_);
    if (answer.storyId.empty() || answer.stickerId.empty() || answer.userId.empty() || answer.rev < 1) return false;
    for (Answer& a : answers_) {
        if (a.storyId != answer.storyId || a.stickerId != answer.stickerId || a.userId != answer.userId) continue;
        if (a.final || answer.rev <= a.rev) return false;  // the latest rev counts; a final one stays
        a = answer;
        save();
        return true;
    }
    answers_.push_back(answer);
    save();
    return true;
}

std::vector<StoryStore::Answer> StoryStore::answers(const std::string& storyId) const {
    std::lock_guard lock(mutex_);
    std::vector<Answer> out;
    for (const Answer& a : answers_) {
        if (a.storyId == storyId) out.push_back(a);
    }
    return out;
}

std::optional<StoryStore::Answer> StoryStore::answer(const std::string& storyId, const std::string& stickerId, const std::string& userId) const {
    std::lock_guard lock(mutex_);
    for (const Answer& a : answers_) {
        if (a.storyId == storyId && a.stickerId == stickerId && a.userId == userId) return a;
    }
    return std::nullopt;
}

void StoryStore::pushDelivery(Delivery delivery) {
    std::lock_guard lock(mutex_);
    if (delivery.id.empty()) delivery.id = util::randomUuid();
    deliveries_.push_back(std::move(delivery));
    save();
}

void StoryStore::updateDelivery(const Delivery& delivery) {
    std::lock_guard lock(mutex_);
    for (Delivery& d : deliveries_) {
        if (d.id != delivery.id) continue;
        d = delivery;
        save();
        return;
    }
}

void StoryStore::removeDelivery(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto before = deliveries_.size();
    std::erase_if(deliveries_, [&](const Delivery& d) { return d.id == id; });
    if (deliveries_.size() != before) save();
}

std::vector<StoryStore::Delivery> StoryStore::deliveries() const {
    std::lock_guard lock(mutex_);
    return deliveries_;
}

bool StoryStore::hasDeliveries() const {
    std::lock_guard lock(mutex_);
    return !deliveries_.empty();
}

// --- Sending ---

namespace {

enum class Outcome { Done, Partial, Waiting };

/// The devices of `userId` for a story message, from a list this device verified (the conversation `listVia` names).
Json storyDevices(V3Device& device, const std::string& storyId, const std::string& userId, const StoryFanOut& fanOut) {
    return verifiedDevices(device, userId, fanOut.listVia ? fanOut.listVia(storyId, userId) : std::string()).devices;
}

/// One pass of one delivery: every listed device of its accounts that doesn't have it yet. Updates `d` in place.
Outcome deliverOnce(V3Device& device, StoryStore::Delivery& d, const StoryFanOut& fanOut, StorySendResult& result) {
    std::vector<Json> recipients;
    std::set<std::string> done(d.done.begin(), d.done.end());
    std::vector<std::string> delivered;
    int unreachable = 0;
    bool complete = true;  // no device of an addressed account left out (already done, or a list that couldn't be read)
    const auto collect = [&] {
        recipients.clear();
        complete = true;
        for (const std::string& userId : d.userIds) {
            // A story id is no conversation: lists are looked up as a friend's (or this account's own).
            Json devices = Json::array();
            try {
                devices = storyDevices(device, d.storyId, userId, fanOut);
            } catch (const EngineError&) {
                ++unreachable;  // their keys changed (held until the person accepts it), or a list that doesn't verify
                complete = false;
                continue;
            } catch (const api::ApiError& error) {
                if (error.status() != 404) throw;  // Tor or the server: the whole delivery waits
                continue;  // not a friend any more: nothing for them
            }
            for (const Json& r : devices) {
                const std::string deviceId = r.value("deviceId", std::string());
                if (userId == device.userId() && deviceId == device.deviceId()) continue;
                if (done.count(userId + "/" + deviceId)) {
                    complete = false;
                    continue;
                }
                recipients.push_back(r);
            }
        }
    };
    FanOut options;
    options.claimVia = fanOut.claimVia;
    options.delivered = &delivered;
    const auto finish = [&](bool queued) {
        for (const std::string& id : delivered) {
            if (done.insert(id).second) d.done.push_back(id);
        }
        result.delivered += static_cast<int>(delivered.size());
        result.unreachable += unreachable;
        if (queued || unreachable == 0) return Outcome::Done;  // queued: the rest waits, encrypted, in the Tor queue
        return Outcome::Partial;
    };
    try {
        // Requests the Tor gate refuses are queued with their ciphertext (send_queue.hpp), never made again.
        SendScope scope(d.id, "messages");
        try {
            collect();
            if (recipients.empty()) return Outcome::Done;
            options.allDevices = complete;  // marked only when nothing was left out (gap 1)
            sendToDevices(device, d.storyId, recipients, d.plaintext, &unreachable, options);
        } catch (const api::ApiError& error) {
            if (error.code() != "device_list_stale") throw;
            // A list changed: what went already is noted; refetch, verify, and the rest once more.
            for (const std::string& id : delivered) done.insert(id);
            unreachable = 0;
            result.delivered += static_cast<int>(delivered.size());
            delivered.clear();
            for (const std::string& id : done) {
                if (std::find(d.done.begin(), d.done.end(), id) == d.done.end()) d.done.push_back(id);
            }
            collect();
            options.allDevices = complete;
            if (!recipients.empty()) sendToDevices(device, d.storyId, recipients, d.plaintext, &unreachable, options);
        }
    } catch (const SendQueued&) {
        return finish(true);
    } catch (const api::ApiError& error) {
        finish(false);
        if (error.code() == "tor_unavailable" || waitsForConnection(error)) return Outcome::Waiting;
        throw;
    }
    return finish(false);
}

/// How many refused sub-deliveries one refused delivery may cost while it's split to find who the server won't take.
constexpr int kStorySplitBudget = 24;

/// `d` for only `users`: a fresh delivery that keeps what already went to those accounts' devices.
StoryStore::Delivery partOf(const StoryStore::Delivery& d, std::vector<std::string> users) {
    StoryStore::Delivery part = d;
    part.id = util::randomUuid();
    part.attempts = 0;
    part.done.clear();
    for (const std::string& id : d.done) {
        const auto slash = id.find('/');
        if (slash != std::string::npos && std::find(users.begin(), users.end(), id.substr(0, slash)) != users.end()) part.done.push_back(id);
    }
    part.userIds = std::move(users);
    return part;
}

/**
 * A delivery the server refused as a whole (the mailbox refuses a batch when one account in it may not get this story
 * id: unfriended, blocked, access revoked). It's split in halves and each half tried, down to single accounts; a single
 * account that's refused is dropped, the others still get it. A story that can't take anyone (deleted, ended) is given
 * up once `budget` refusals are spent. What has to wait or be tried again is stored as its own delivery.
 */
void splitRefused(V3Device& device, StoryStore& store, const StoryStore::Delivery& d, const StoryFanOut& fanOut, StorySendResult& result,
                  int& budget) {
    if (d.userIds.size() < 2 || budget <= 0) return;
    const auto mid = d.userIds.begin() + static_cast<std::ptrdiff_t>(d.userIds.size() / 2);
    for (auto users : {std::vector<std::string>(d.userIds.begin(), mid), std::vector<std::string>(mid, d.userIds.end())}) {
        StoryStore::Delivery part = partOf(d, std::move(users));
        if (result.waiting) {
            store.pushDelivery(std::move(part));  // Tor or the server went away: the rest waits, untried
            continue;
        }
        Outcome outcome;
        try {
            outcome = deliverOnce(device, part, fanOut, result);
        } catch (const std::exception&) {
            --budget;
            splitRefused(device, store, part, fanOut, result, budget);
            continue;
        }
        if (outcome == Outcome::Done) continue;
        if (outcome == Outcome::Waiting) result.waiting = true;
        else part.attempts = 1;
        store.pushDelivery(std::move(part));
    }
}

StorySendResult drain(V3Device& device, StoryStore& store, const StoryFanOut& fanOut) {
    StorySendResult result;
    store.dropStaleDeliveries();
    for (StoryStore::Delivery d : store.deliveries()) {
        const std::size_t before = d.done.size();
        Outcome outcome;
        try {
            outcome = deliverOnce(device, d, fanOut, result);
        } catch (const std::exception&) {
            // Refused (the server's story rule, a malformed list): it doesn't block the rest. With several accounts in it,
            // the ones the server still takes get it (one refused account no longer costs everyone the key).
            store.removeDelivery(d.id);
            int budget = kStorySplitBudget;
            splitRefused(device, store, d, fanOut, result, budget);
            if (result.waiting) return result;
            continue;
        }
        if (outcome == Outcome::Done) {
            store.removeDelivery(d.id);
            continue;
        }
        if (outcome == Outcome::Waiting) {
            store.updateDelivery(d);  // what went is kept; the rest waits, in order
            result.waiting = true;
            return result;
        }
        // Some devices had no session and no key to claim: tried again on later passes, a few times.
        d.attempts = d.done.size() > before ? 0 : d.attempts + 1;
        if (d.attempts >= kStoryDeliveryAttempts) {
            store.removeDelivery(d.id);
        } else {
            store.updateDelivery(d);
        }
    }
    return result;
}

std::vector<std::string> withSelf(std::vector<std::string> users, const std::string& self) {
    std::vector<std::string> out;
    for (std::string& u : users) {
        if (u != self && std::find(out.begin(), out.end(), u) == out.end()) out.push_back(std::move(u));
    }
    out.push_back(self);  // this account's other devices
    return out;
}

}  // namespace

StorySendResult sendStoryKey(V3Device& device, StoryStore& store, const std::string& storyId, const crypto::SecretBytes& key,
                             std::int64_t expiresAt, const std::vector<std::string>& viewers, const StoryFanOut& fanOut, bool queueOnly) {
    const std::string plaintext = encodeStoryKey(storyId, key, expiresAt, util::randomUuid(), util::nowMillis());
    store.putKey(storyId, device.userId(), key, expiresAt, true);
    StoryStore::Delivery d;
    d.id = util::randomUuid();
    d.storyId = storyId;
    d.kind = "storykey";
    d.userIds = withSelf(viewers, device.userId());
    d.plaintext = plaintext;
    d.until = expiresAt;
    store.pushDelivery(std::move(d));
    if (queueOnly) {
        StorySendResult waiting;
        waiting.waiting = true;
        return waiting;
    }
    return drain(device, store, fanOut);
}

StorySendResult sendStoryAnswer(V3Device& device, StoryStore& store, const std::string& storyId, const std::string& authorId,
                                const std::string& stickerId, bool final, const Json& payload, std::int64_t expiresAt, const StoryFanOut& fanOut,
                                bool queueOnly) {
    if (authorId.empty() || authorId == device.userId()) throw EngineError("bad_answer", "An answer goes to the story's author.");
    const auto previous = store.answer(storyId, stickerId, device.userId());
    if (previous && previous->final) throw EngineError("answer_final", "You already answered that.");
    const std::int64_t rev = previous ? previous->rev + 1 : 1;
    const std::int64_t ts = util::nowMillis();
    const std::string plaintext = encodeStoryAnswer(storyId, stickerId, rev, final, payload, util::randomUuid(), ts);
    // Kept here first: this device shows its own answer, and the next one counts on from this rev.
    store.putAnswer(StoryStore::Answer{storyId, stickerId, device.userId(), rev, final, payload.dump(), ts});
    StoryStore::Delivery d;
    d.id = util::randomUuid();
    d.storyId = storyId;
    d.kind = "storyanswer";
    d.userIds = withSelf({authorId}, device.userId());
    d.plaintext = plaintext;
    d.until = expiresAt;
    store.pushDelivery(std::move(d));
    if (queueOnly) {
        StorySendResult waiting;
        waiting.waiting = true;
        return waiting;
    }
    return drain(device, store, fanOut);
}

StorySendResult sendQueuedStories(V3Device& device, StoryStore& store, const StoryFanOut& fanOut) {
    return drain(device, store, fanOut);
}

int fillStoryGaps(V3Device& device, StoryStore& store, const std::vector<StoryGap>& gaps,
                  const std::function<void(const std::string& storyId, const std::vector<std::string>& userIds)>& addAccess,
                  const StoryFanOut& fanOut) {
    int handed = 0;
    for (const StoryGap& gap : gaps) {
        const auto own = store.key(gap.storyId, device.userId());
        // (A late friend's key outlives the story's end only for a highlighted story; the server decides that.)
        if (!own || !own->own || gap.userIds.empty()) continue;
        std::vector<std::string> users;
        for (const std::string& u : gap.userIds) {
            if (u != device.userId() && std::find(users.begin(), users.end(), u) == users.end()) users.push_back(u);
        }
        if (users.empty()) continue;
        // Access first: the mailbox refuses a story key for an account without it (12-part-d-stories.md, "API (as built)").
        try {
            addAccess(gap.storyId, users);
        } catch (const std::exception&) {
            continue;  // not a friend any more, the story ended...: tried again with the next list of gaps
        }
        const std::int64_t until = std::max<std::int64_t>(own->expiresAt, store.now() + std::int64_t{24} * 3600 * 1000);
        StoryStore::Delivery d;
        d.id = util::randomUuid();
        d.storyId = gap.storyId;
        d.kind = "storykey";
        d.userIds = users;
        d.plaintext = encodeStoryKey(gap.storyId, *own->key, own->expiresAt, util::randomUuid(), util::nowMillis());
        d.until = until;
        store.pushDelivery(std::move(d));
        handed += static_cast<int>(users.size());
    }
    if (handed > 0) drain(device, store, fanOut);
    return handed;
}

// --- Anonymous answers (RVSD) ---

StorySealer engineSealer(V3Device& device, const StoryFanOut& fanOut) {
    return [&device, fanOut](const std::string& storyId, const std::string& authorId, crypto::ByteView payload) {
        // The author's devices from their verified list, and the Signal certificate each one declared.
        const std::string via = fanOut.listVia ? fanOut.listVia(storyId, authorId) : std::string();
        const Json devices = verifiedDevices(device, authorId, via).devices;
        Json certificates = Json::array();
        try {
            certificates = device.server().signalCertificates(authorId, via);
        } catch (const api::ApiError& error) {
            if (error.code() == "tor_unavailable" || waitsForConnection(error)) throw;
        }
        std::vector<crypto::stories::RvsdCopy> copies;
        for (const Json& d : devices) {
            if (copies.size() == crypto::stories::kRvsdMaxDevices) break;
            const std::string deviceId = d.value("deviceId", std::string());
            Json certificate;
            for (const Json& c : certificates) {
                if (c.is_object() && c.value("deviceId", std::string()) == deviceId) certificate = c.value("certificate", Json(nullptr));
            }
            if (certificate.is_null()) continue;
            // The engine seals to the device's Signal identity key from its certificate, and refuses a certificate
            // that isn't the one pinned for that device in its verified list. No key ever leaves the engine.
            const Json sealed = device.op("storySeal", Json{{"certificate", certificate}, {"plaintext", crypto::base64Encode(payload)}});
            if (!sealed.value("ok", false)) continue;
            try {
                copies.push_back(crypto::stories::RvsdCopy{deviceId, crypto::base64Decode(stringAt(sealed, "sealed"))});
            } catch (const std::exception&) {
            }
        }
        return copies;
    };
}

crypto::Bytes sealStoryAnswer(const std::string& storyId, const std::string& authorId, const model::stories::Sticker& sticker,
                              const model::stories::Answer& answer, const StorySealer& seal) {
    const crypto::Bytes payload = crypto::stories::anonymousAnswerPayload(storyId, sticker, answer);
    const auto copies = seal(storyId, authorId, payload);
    if (copies.empty()) throw EngineError("story_seal_unavailable", "This answer can't be sealed to their devices yet.");
    return crypto::stories::encodeRvsd(copies);
}

SealedOpener engineOpener(V3Device& device) {
    return [&device](crypto::ByteView sealed) -> std::optional<crypto::Bytes> {
        // Opened with this device's own Signal identity key, inside the engine. An engine without Signal opens nothing.
        const Json answer = device.op("storyUnseal", Json{{"sealed", crypto::base64Encode(sealed)}});
        if (!answer.value("ok", false)) return std::nullopt;
        try {
            return crypto::base64Decode(stringAt(answer, "plaintext"));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
}

std::optional<model::stories::Answer> openStoryAnswer(const std::string& deviceId, const std::string& storyId, const model::stories::Sticker& sticker,
                                                      crypto::ByteView rvsd, const SealedOpener& open) {
    const auto copies = crypto::stories::parseRvsd(rvsd);
    if (!copies || !open) return std::nullopt;
    for (const auto& c : *copies) {
        if (c.deviceId != deviceId) continue;
        const auto payload = open(c.sealed);
        if (!payload) return std::nullopt;
        try {
            return crypto::stories::openAnonymousPayload(storyId, sticker, *payload);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace redvault::e2ev3
