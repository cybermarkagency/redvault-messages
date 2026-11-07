#include "redvault/e2ev3/device_store.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>

#include "fs_util.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/util/text.hpp"

namespace redvault::e2ev3 {

using Json = nlohmann::json;

DeviceStore::DeviceStore(std::filesystem::path dir, std::shared_ptr<KeyVault> vault) : dir_(std::move(dir)), vault_(std::move(vault)) {}

std::filesystem::path DeviceStore::folder(const std::string& userId) const {
    return dir_ / crypto::toHex(crypto::sha256(crypto::toBytes(userId))).substr(0, 32);
}

OpenDevice DeviceStore::create(const std::string& userId) {
    // Replaces a device this computer already had for the account, key and all.
    wipe(userId);
    const std::string deviceId = util::randomUuid();
    crypto::SecretBytes key = crypto::SecretBytes::copyOf(crypto::randomBytes(32));
    vault_->put(vaultId(userId, deviceId), key.view());
    const Json meta = {{"userId", userId}, {"deviceId", deviceId}, {"protection", static_cast<int>(vault_->protection())}};
    fsutil::writeAtomic(folder(userId) / "device.json", meta.dump());
    return OpenDevice{deviceId, crypto::base64Encode(key.view()), std::nullopt};
}

std::optional<OpenDevice> DeviceStore::load(const std::string& userId) {
    const auto metaText = fsutil::readAll(folder(userId) / "device.json");
    if (!metaText) return std::nullopt;
    const Json meta = Json::parse(*metaText, nullptr, false);
    if (!meta.is_object() || meta.value("userId", std::string()) != userId || !meta.contains("deviceId")) return std::nullopt;
    const std::string deviceId = meta.at("deviceId").get<std::string>();
    // A folder written under another protection can't be read by this vault: treat as lost, never as a crash.
    if (meta.value("protection", -1) != static_cast<int>(vault_->protection())) return std::nullopt;
    const auto key = vault_->get(vaultId(userId, deviceId));
    if (!key || key->size() != 32) return std::nullopt;
    std::optional<std::string> state = fsutil::readAll(folder(userId) / "state");
    if (state && state->empty()) state.reset();
    return OpenDevice{deviceId, crypto::base64Encode(key->view()), std::move(state)};
}

void DeviceStore::saveState(const std::string& userId, const std::string& deviceId, const std::string& state) {
    const auto metaText = fsutil::readAll(folder(userId) / "device.json");
    const Json meta = metaText ? Json::parse(*metaText, nullptr, false) : Json();
    if (!meta.is_object() || meta.value("deviceId", std::string()) != deviceId) {
        throw std::runtime_error("this computer's device was replaced or removed");
    }
    fsutil::writeAtomic(folder(userId) / "state", state);
}

void DeviceStore::wipe(const std::string& userId) {
    if (const auto metaText = fsutil::readAll(folder(userId) / "device.json")) {
        const Json meta = Json::parse(*metaText, nullptr, false);
        if (meta.is_object() && meta.contains("deviceId") && meta.at("deviceId").is_string()) {
            vault_->erase(vaultId(userId, meta.at("deviceId").get<std::string>()));
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(folder(userId), ec);
}

}  // namespace redvault::e2ev3
