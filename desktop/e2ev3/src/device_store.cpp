// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/device_store.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include "fs_util.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/util/text.hpp"

namespace redvault::e2ev3 {

using Json = nlohmann::json;

namespace {

constexpr int kStateFormat = 2;  // {"v":2,"counter":N,"state":"...","mac":"..."}; before counters the file was the bare engine state
constexpr std::size_t kKeptEvents = 20;

/// The key that authenticates the counter, from the device's storage key (BLAKE2b, keyed, like V3Device::deriveKey).
crypto::Bytes counterKey(const std::string& storageKeyBase64) {
    const crypto::Bytes key = crypto::base64Decode(storageKeyBase64);
    static constexpr std::string_view context = "redvault-v3-state-counter";
    crypto::Bytes out(32);
    crypto_generichash(out.data(), out.size(), reinterpret_cast<const unsigned char*>(context.data()), context.size(), key.data(), key.size());
    return out;
}

crypto::Bytes hmac(const crypto::Bytes& key, const std::string& message) {
    crypto::Bytes out(crypto_auth_hmacsha256_BYTES);
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, key.data(), key.size());
    crypto_auth_hmacsha256_update(&st, reinterpret_cast<const unsigned char*>(message.data()), message.size());
    crypto_auth_hmacsha256_final(&st, out.data());
    sodium_memzero(&st, sizeof st);
    return out;
}

/// What a MAC covers, NUL-separated: a label, which account and device, the counter, and (for the state file) the state.
std::string macMessage(std::string_view label, const std::string& userId, const std::string& deviceId, std::uint64_t counter,
                       const std::string* state = nullptr) {
    std::string m(label);
    m += '\0';
    m += userId;
    m += '\0';
    m += deviceId;
    m += '\0';
    m += std::to_string(counter);
    if (state) {
        m += '\0';
        m += *state;
    }
    return m;
}

struct StoredState {
    std::uint64_t counter = 0;
    std::string state;
    bool verified = true;  // false: a counter or state whose MAC doesn't check out (edited, or another device's)
    bool legacy = false;   // saved before counters existed: the bare engine state, no counter or MAC
};

/// The state file as written: the current format, or the bare state written before counters (counter 0).
StoredState parseState(const std::string& text, const std::string& userId, const std::string& deviceId, const crypto::Bytes& key) {
    // The engine's state is base64, never a JSON object: anything that parses as one is the current format.
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return StoredState{0, text, true, true};
    StoredState out;
    if (j.value("v", 0) != kStateFormat || !j.contains("counter") || !j.at("counter").is_number_unsigned() || !j.contains("state") ||
        !j.at("state").is_string() || !j.contains("mac") || !j.at("mac").is_string()) {
        out.verified = false;
        return out;
    }
    out.counter = j.at("counter").get<std::uint64_t>();
    out.state = j.at("state").get<std::string>();
    try {
        const crypto::Bytes mac = crypto::base64Decode(j.at("mac").get<std::string>());
        out.verified = crypto::constantTimeEquals(mac, hmac(key, macMessage("redvault-v3-state", userId, deviceId, out.counter, &out.state)));
    } catch (const std::exception&) {
        out.verified = false;
    }
    return out;
}

std::string formatState(const std::string& userId, const std::string& deviceId, std::uint64_t counter, const std::string& state,
                        const crypto::Bytes& key) {
    return Json{{"v", kStateFormat},
                {"counter", counter},
                {"state", state},
                {"mac", crypto::base64Encode(hmac(key, macMessage("redvault-v3-state", userId, deviceId, counter, &state)))}}
        .dump();
}

/// The vault's copy: 8 bytes of counter (big-endian) and their MAC. Nothing when absent or not this device's.
std::optional<std::uint64_t> parseMirror(const std::optional<crypto::SecretBytes>& raw, const std::string& userId, const std::string& deviceId,
                                         const crypto::Bytes& key) {
    if (!raw || raw->size() != 8 + crypto_auth_hmacsha256_BYTES) return std::nullopt;
    const crypto::ByteView bytes = raw->view();
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    if (!crypto::constantTimeEquals(bytes.subspan(8), hmac(key, macMessage("redvault-v3-counter", userId, deviceId, value)))) return std::nullopt;
    return value;
}

crypto::Bytes formatMirror(const std::string& userId, const std::string& deviceId, std::uint64_t value, const crypto::Bytes& key) {
    crypto::Bytes out(8);
    for (std::size_t i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * (7 - i)));
    const crypto::Bytes mac = hmac(key, macMessage("redvault-v3-counter", userId, deviceId, value));
    out.insert(out.end(), mac.begin(), mac.end());
    return out;
}

}  // namespace

DeviceStore::DeviceStore(std::filesystem::path dir, std::shared_ptr<KeyVault> vault) : dir_(std::move(dir)), vault_(std::move(vault)) {}

std::filesystem::path DeviceStore::folder(const std::string& userId) const {
    return dir_ / crypto::toHex(crypto::sha256(crypto::toBytes(userId))).substr(0, 32);
}

std::filesystem::path DeviceStore::eventsFile(const std::string& userId) const {
    // Beside the account's folder, not in it: replacing or wiping the device keeps the record of why.
    auto file = folder(userId);
    file += ".events";
    return file;
}

OpenDevice DeviceStore::create(const std::string& userId) {
    // Replaces a device this computer already had for the account, key and all.
    wipe(userId);
    const std::string deviceId = util::randomUuid();
    crypto::SecretBytes key = crypto::SecretBytes::copyOf(crypto::randomBytes(32));
    vault_->put(vaultId(userId, deviceId), key.view());
    const Json meta = {{"userId", userId}, {"deviceId", deviceId}, {"protection", static_cast<int>(vault_->protection())}};
    fsutil::writeAtomic(folder(userId) / "device.json", meta.dump());
    OpenDevice made{deviceId, crypto::base64Encode(key.view()), std::nullopt};
    Counter counter{deviceId, counterKey(made.storageKey), 0};
    // The vault's copy exists from the start, so a state that carries a counter always has one to be checked against.
    writeMirror(userId, counter);
    std::lock_guard lock(mutex_);
    counters_[userId] = std::move(counter);
    return made;
}

std::optional<OpenDevice> DeviceStore::load(const std::string& userId) {
    const auto metaText = fsutil::readAll(folder(userId) / "device.json");
    if (!metaText) return std::nullopt;
    const Json meta = Json::parse(*metaText, nullptr, false);
    if (!meta.is_object() || meta.value("userId", std::string()) != userId || !meta.contains("deviceId") || !meta.at("deviceId").is_string()) {
        return std::nullopt;
    }
    const std::string deviceId = meta.at("deviceId").get<std::string>();
    // A folder written under another protection can't be read by this vault: treat as lost, never as a crash.
    if (meta.value("protection", -1) != static_cast<int>(vault_->protection())) return std::nullopt;
    const auto key = vault_->get(vaultId(userId, deviceId));
    if (!key || key->size() != 32) return std::nullopt;
    OpenDevice out{deviceId, crypto::base64Encode(key->view()), std::nullopt};
    Counter counter{deviceId, counterKey(out.storageKey), 0};

    std::lock_guard lock(mutex_);
    const std::optional<std::string> text = fsutil::readAll(folder(userId) / "state");
    if (!text || text->empty()) {
        // Made, but the engine hasn't saved a state yet: nothing to roll back.
        counters_[userId] = std::move(counter);
        return out;
    }
    const StoredState stored = parseState(*text, userId, deviceId, counter.macKey);
    const std::optional<std::uint64_t> mirror = parseMirror(vault_->get(counterId(userId, deviceId)), userId, deviceId, counter.macKey);
    if (!stored.verified) {
        record(userId, StoreEvent{"state_unverified", 0, deviceId, stored.counter, mirror.value_or(0)});
        counters_.erase(userId);
        return std::nullopt;
    }
    if (!mirror && !stored.legacy) {
        // A state with a counter always has the vault's copy (made with the device): without it, deleting that copy
        // and restoring an old state file would pass. Refused like an older copy.
        record(userId, StoreEvent{"counter_missing", 0, deviceId, stored.counter, 0});
        counters_.erase(userId);
        return std::nullopt;
    }
    if (mirror && stored.counter < *mirror) {
        record(userId, StoreEvent{"state_rolled_back", 0, deviceId, stored.counter, *mirror});
        counters_.erase(userId);
        return std::nullopt;
    }
    counter.value = stored.counter;
    // Missing only for a state saved before counters (initialised here), or behind (a crash between the two
    // writes): bring the vault's copy up.
    if (!mirror || *mirror < stored.counter) writeMirror(userId, counter);
    counters_[userId] = std::move(counter);
    out.state = stored.state;
    return out;
}

void DeviceStore::saveState(const std::string& userId, const std::string& deviceId, const std::string& state) {
    const auto metaText = fsutil::readAll(folder(userId) / "device.json");
    const Json meta = metaText ? Json::parse(*metaText, nullptr, false) : Json();
    if (!meta.is_object() || meta.value("deviceId", std::string()) != deviceId) {
        throw std::runtime_error("this computer's device was replaced or removed");
    }
    std::lock_guard lock(mutex_);
    auto it = counters_.find(userId);
    if (it == counters_.end() || it->second.deviceId != deviceId) {
        // Not opened through this store (the app always opens it first): start from the highest counter seen.
        const auto key = vault_->get(vaultId(userId, deviceId));
        if (!key || key->size() != 32) throw std::runtime_error("this computer's device key is gone");
        Counter counter{deviceId, counterKey(crypto::base64Encode(key->view())), 0};
        const auto mirror = parseMirror(vault_->get(counterId(userId, deviceId)), userId, deviceId, counter.macKey);
        std::uint64_t onDisk = 0;
        if (const auto text = fsutil::readAll(folder(userId) / "state"); text && !text->empty()) {
            const StoredState stored = parseState(*text, userId, deviceId, counter.macKey);
            if (stored.verified) onDisk = stored.counter;
        }
        counter.value = std::max(onDisk, mirror.value_or(0));
        it = counters_.insert_or_assign(userId, std::move(counter)).first;
    }
    Counter next = it->second;
    next.value += 1;
    // The state first, then the counter: a crash in between leaves the state ahead, which load() accepts.
    fsutil::writeAtomic(folder(userId) / "state", formatState(userId, deviceId, next.value, state, next.macKey));
    it->second.value = next.value;
    try {
        writeMirror(userId, next);
    } catch (const std::exception&) {
        // The state is saved; the vault's copy catches up on the next save or load.
    }
}

void DeviceStore::writeMirror(const std::string& userId, const Counter& counter) {
    const crypto::Bytes value = formatMirror(userId, counter.deviceId, counter.value, counter.macKey);
    vault_->put(counterId(userId, counter.deviceId), value);
}

void DeviceStore::wipe(const std::string& userId) {
    if (const auto metaText = fsutil::readAll(folder(userId) / "device.json")) {
        const Json meta = Json::parse(*metaText, nullptr, false);
        if (meta.is_object() && meta.contains("deviceId") && meta.at("deviceId").is_string()) {
            const std::string deviceId = meta.at("deviceId").get<std::string>();
            vault_->erase(vaultId(userId, deviceId));
            vault_->erase(counterId(userId, deviceId));
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(folder(userId), ec);
    std::lock_guard lock(mutex_);
    counters_.erase(userId);
}

void DeviceStore::record(const std::string& userId, StoreEvent event) {
    std::vector<StoreEvent> kept = events(userId);
    // The same refusal seen again on the next load is one event, not one per load.
    if (!kept.empty()) {
        const StoreEvent& last = kept.back();
        if (last.kind == event.kind && last.deviceId == event.deviceId && last.stateCounter == event.stateCounter &&
            last.storedCounter == event.storedCounter) {
            return;
        }
    }
    event.atMs = util::nowMillis();
    kept.push_back(std::move(event));
    if (kept.size() > kKeptEvents) kept.erase(kept.begin(), kept.end() - static_cast<std::ptrdiff_t>(kKeptEvents));
    Json out = Json::array();
    for (const StoreEvent& e : kept) {
        out.push_back(Json{{"kind", e.kind}, {"at", e.atMs}, {"deviceId", e.deviceId}, {"stateCounter", e.stateCounter}, {"storedCounter", e.storedCounter}});
    }
    try {
        fsutil::writeAtomic(eventsFile(userId), out.dump());
    } catch (const std::exception&) {
        // Only a record: the refusal doesn't depend on it.
    }
}

std::vector<StoreEvent> DeviceStore::events(const std::string& userId) const {
    std::vector<StoreEvent> out;
    const auto text = fsutil::readAll(eventsFile(userId));
    if (!text) return out;
    const Json j = Json::parse(*text, nullptr, false);
    if (!j.is_array()) return out;
    for (const Json& e : j) {
        if (!e.is_object()) continue;
        out.push_back(StoreEvent{e.value("kind", std::string()), e.value("at", std::int64_t{0}), e.value("deviceId", std::string()),
                                 e.value("stateCounter", std::uint64_t{0}), e.value("storedCounter", std::uint64_t{0})});
    }
    return out;
}

}  // namespace redvault::e2ev3
