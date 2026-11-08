// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/data/vault_repository.hpp"

#include <map>

#include "redvault/api/api_error.hpp"
#include "redvault/api/drive_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/vault_crypto.hpp"
#include "redvault/net/http.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

using api::ApiError;
using api::UserFacingError;
using Json = nlohmann::json;

namespace {

std::int64_t revOf(const Json& data) {
    const auto rev = data.find("rev");
    return rev != data.end() && rev->is_number_integer() ? rev->get<std::int64_t>() : 0;
}

template <typename F>
auto mapErrors(const std::string& fallback, F&& block) -> decltype(block()) {
    try {
        return block();
    } catch (const UserFacingError&) {
        throw;
    } catch (const ApiError& e) {
        if (e.status() == 401) throw;
        if (e.code() == "vault_conflict") {
            throw UserFacingError("This item was changed on another device. It's been reloaded: make your change again.");
        }
        if (e.code() == "network_error") throw UserFacingError("Network interrupted. Try again.");
        if (e.code() == "rate_limited") throw UserFacingError(e.what());
        if (e.status() == 404) throw UserFacingError("That item isn't there anymore.");
        throw UserFacingError(fallback);
    } catch (const net::NetworkError&) {
        throw UserFacingError("Network interrupted. Try again.");
    } catch (const crypto::CryptoError&) {
        throw UserFacingError(fallback);
    } catch (const Json::exception&) {
        throw UserFacingError(fallback);
    }
}

}  // namespace

std::int64_t MemoryVaultRevs::seen(const std::string& userId, const std::string& name) {
    std::lock_guard lock(mutex_);
    const auto it = seen_.find(userId + ":" + name);
    return it == seen_.end() ? 0 : it->second;
}

void MemoryVaultRevs::record(const std::string& userId, const std::string& name, std::int64_t rev) {
    std::lock_guard lock(mutex_);
    seen_[userId + ":" + name] = rev;
}

VaultRepository::VaultRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const SelfIdentity> identity, std::shared_ptr<VaultRevs> revs,
                                 std::shared_ptr<ContactKeyPins> contacts)
    : api_(std::move(api)),
      identity_(std::move(identity)),
      revs_(revs ? std::move(revs) : std::make_shared<MemoryVaultRevs>()),
      contacts_(std::move(contacts)) {}

bool VaultRepository::accept(const std::string& id, const Json& data, bool shared) {
    const std::string record = util::toLowerAscii(id);
    if (const auto bound = data.find("boundId"); bound != data.end() && !bound->is_null()) {
        if (!bound->is_string() || util::toLowerAscii(bound->get<std::string>()) != record) return false;
    }
    const std::string name = shared ? "shared:" + record : record;
    const std::string account = util::toLowerAscii(identity_->userId);
    const std::int64_t rev = revOf(data);
    const std::int64_t seen = revs_->seen(account, name);
    if (rev < seen) return false;
    if (rev > seen) revs_->record(account, name, rev);
    return true;
}

bool VaultRepository::keyUnchanged(const std::string& recipientId, const std::string& publicKeyB64) {
    return contacts_ == nullptr || !contacts_->keyChanged(identity_->userId, recipientId, publicKeyB64);
}

std::vector<VaultEntry> VaultRepository::list() {
    return mapErrors("Couldn't load your vault. Try again.", [&] {
        const auto key = crypto::vault::vaultKey(identity_->privateKey.view());
        std::map<std::string, std::vector<std::string>> sharedWith;
        for (const auto& s : api::vault::shares(*api_)) sharedWith[s.itemId].push_back(s.recipientId);

        int refused = 0;
        std::vector<VaultEntry> out;
        for (const auto& item : api::vault::list(*api_)) {
            try {
                VaultEntry entry;
                entry.id = item.id;
                entry.revision = item.revision;
                entry.data = Json::parse(
                    crypto::vault::openItem(crypto::base64Decode(item.ciphertext), crypto::base64Decode(item.nonce), key.view()));
                entry.sharedWith = sharedWith[item.id];
                if (!accept(entry.id, entry.data, false)) {
                    ++refused;
                    continue;
                }
                out.push_back(std::move(entry));
            } catch (const std::exception&) {
                // Sealed under another key (never expected): left out rather than shown broken.
            }
        }
        for (const auto& shared : api::vault::sharedWithMe(*api_)) {
            try {
                VaultEntry entry;
                entry.id = shared.itemId;
                entry.incoming = true;
                entry.ownerId = shared.ownerId;
                entry.data = Json::parse(crypto::vault::openFromSender(crypto::base64Decode(shared.sealed), identity_->publicKey,
                                                                        identity_->privateKey.view()));
                if (!accept(entry.id, entry.data, true)) {
                    ++refused;
                    continue;
                }
                out.push_back(std::move(entry));
            } catch (const std::exception&) {
            }
        }
        refused_ = refused;
        return out;
    });
}

VaultEntry VaultRepository::save(VaultEntry entry) {
    return mapErrors("Couldn't save the item. Try again.", [&] {
        if (entry.incoming) throw UserFacingError("Items shared with you are read-only.");
        const auto now = util::nowMillis();
        if (!entry.data.contains("created")) entry.data["created"] = now;
        entry.data["updated"] = now;
        entry.data["v"] = 1;
        const auto key = crypto::vault::vaultKey(identity_->privateKey.view());
        const auto write = [&](const Json& data, const std::string& id, int revision) {
            const auto sealed = crypto::vault::sealItem(data.dump(), key.view());
            const auto ciphertext = crypto::base64Encode(sealed.ciphertext);
            const auto nonce = crypto::base64Encode(sealed.nonce);
            return id.empty() ? api::vault::create(*api_, ciphertext, nonce) : api::vault::update(*api_, id, revision, ciphertext, nonce);
        };
        if (entry.id.empty()) {
            // The server picks the id, so the item is bound in a second write. If that one fails, its next save does it.
            auto stored = write(entry.data, {}, 0);
            Json bound = entry.data;
            bound["boundId"] = stored.id;
            bound["rev"] = 1;
            try {
                stored = write(bound, stored.id, stored.revision);
                entry.data = std::move(bound);
            } catch (const ApiError& e) {
                if (e.status() == 401) throw;
            } catch (const net::NetworkError&) {
            }
            entry.id = stored.id;
            entry.revision = stored.revision;
        } else {
            entry.data["boundId"] = entry.id;
            entry.data["rev"] = revOf(entry.data) + 1;
            const auto stored = write(entry.data, entry.id, entry.revision);
            entry.id = stored.id;
            entry.revision = stored.revision;
        }
        accept(entry.id, entry.data, false);
        const std::string json = entry.data.dump();
        // Everyone it's shared with gets the new version.
        for (const auto& recipientId : entry.sharedWith) {
            const auto recipient = api_->getUser(recipientId);
            if (!keyUnchanged(recipientId, recipient.publicKey)) continue;  // Their copy waits until the new safety number is checked.
            const auto copy = crypto::vault::sealForRecipient(json, crypto::base64Decode(recipient.publicKey));
            api::vault::share(*api_, entry.id, recipientId, crypto::base64Encode(copy));
        }
        return entry;
    });
}

void VaultRepository::remove(const VaultEntry& entry) {
    mapErrors("Couldn't delete the item. Try again.", [&] { api::vault::remove(*api_, entry.id); });
}

void VaultRepository::share(const VaultEntry& entry, const std::string& recipientId) {
    mapErrors("Couldn't share the item. Try again.", [&] {
        const auto recipient = api_->getUser(recipientId);
        if (!keyUnchanged(recipientId, recipient.publicKey)) throw UserFacingError(KeyTrust::kContactKeyChanged);
        const auto copy = crypto::vault::sealForRecipient(entry.data.dump(), crypto::base64Decode(recipient.publicKey));
        api::vault::share(*api_, entry.id, recipientId, crypto::base64Encode(copy));
    });
}

void VaultRepository::unshare(const VaultEntry& entry, const std::string& recipientId) {
    mapErrors("Couldn't stop sharing the item. Try again.", [&] { api::vault::unshare(*api_, entry.id, recipientId); });
}

std::int64_t VaultRepository::breachCount(const std::string& password) {
    if (password.empty()) return 0;
    try {
        const std::string hash = crypto::vault::breachHash(password);
        const std::string range = api::vault::breachRange(*api_, hash.substr(0, 5));
        return crypto::vault::breachCount(hash, range);
    } catch (const ApiError& e) {
        if (e.status() == 401) throw;
        return -1;
    } catch (const std::exception&) {
        return -1;
    }
}

}  // namespace redvault::data
