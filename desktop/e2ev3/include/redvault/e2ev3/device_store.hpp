// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "redvault/e2ev3/key_vault.hpp"

// This computer's protocol-3 device for each account (docs/multi-device-threat-model.md
// section 5): the engine's encrypted state in a file, and the 32-byte storage key
// it's encrypted under, which only the KeyVault holds. Neither ever goes to the
// server. Signing out of the device (Settings > Device keys > Delete) removes both.
//
// Layout under `dir`: <hash of userId>/device.json (ids, which protection) and
// <hash of userId>/state (the engine's state, base64). Losing the vault entry
// (a reset Windows profile, a new keyring) loses the device: it must be linked
// again, which isn't a lockout while another device or the recovery key exists.

namespace redvault::e2ev3 {

struct OpenDevice {
    std::string deviceId;
    /// base64; handed to the engine with each call, never written anywhere unprotected.
    std::string storageKey;
    /// The engine's state (base64), or nothing until the engine made one.
    std::optional<std::string> state;
};

class DeviceStore {
public:
    DeviceStore(std::filesystem::path dir, std::shared_ptr<KeyVault> vault);

    /// A new device for `userId` here: a random device id and storage key. Replaces any device already kept for the account.
    OpenDevice create(const std::string& userId);
    /// This computer's device for `userId`, or nothing when there is none (or its key can't be recovered any more).
    std::optional<OpenDevice> load(const std::string& userId);
    /// Saves the engine's new state. Called before anything that call produced is sent.
    void saveState(const std::string& userId, const std::string& deviceId, const std::string& state);
    /// Forgets the device: its key and its protocol-3 history here.
    void wipe(const std::string& userId);

    Protection protection() const { return vault_->protection(); }

private:
    std::filesystem::path folder(const std::string& userId) const;
    static std::string vaultId(const std::string& userId, const std::string& deviceId) { return userId + "/" + deviceId; }

    std::filesystem::path dir_;
    std::shared_ptr<KeyVault> vault_;
};

}  // namespace redvault::e2ev3
