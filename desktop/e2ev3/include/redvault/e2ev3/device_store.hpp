// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
//
// Rollback detection (docs/signal-integration/07-step1-default-path.md section 4):
// every save stores `counter = previous + 1` with the state, authenticated by an
// HMAC-SHA256 under a key derived from the storage key (so editing the counter
// or the state is detected), and then the same counter in a second vault entry
// ("<userId>/<deviceId>/counter": its own DPAPI blob, Secret Service item or
// 0600 file) that a restore of the state folder alone doesn't bring back. The
// state is always written first, the counter second. On load:
//   state counter lower than the vault's  -> refused (the device is "not linked"),
//                                            and a local event is recorded
//   equal                                 -> fine
//   higher (a crash between the writes)   -> accepted, the vault's is raised
//   no vault entry, for a state saved before counters -> initialised from the state
//   no vault entry (or one that doesn't verify) for a state with a counter -> refused
//                                            like an older copy: create() writes the entry
//                                            first, so deleting it is the only way to lose it
// A restore of BOTH the state and the vault entry (a full-disk image) isn't
// detected: that would need a hardware counter, which apps don't get here.

namespace redvault::e2ev3 {

struct OpenDevice {
    std::string deviceId;
    /// base64; handed to the engine with each call, never written anywhere unprotected.
    std::string storageKey;
    /// The engine's state (base64), or nothing until the engine made one.
    std::optional<std::string> state;
};

/// Something this computer noticed about its stored device, kept only here (Settings shows it).
struct StoreEvent {
    /// "state_rolled_back": the state is older than its counter; "state_unverified": the state's counter doesn't verify;
    /// "counter_missing": a state with a counter, but the vault's copy is gone.
    std::string kind;
    std::int64_t atMs = 0;
    std::string deviceId;
    std::uint64_t stateCounter = 0;
    std::uint64_t storedCounter = 0;
};

class DeviceStore {
public:
    DeviceStore(std::filesystem::path dir, std::shared_ptr<KeyVault> vault);

    /// A new device for `userId` here: a random device id and storage key. Replaces any device already kept for the account.
    OpenDevice create(const std::string& userId);
    /// This computer's device for `userId`, or nothing when there is none, its key can't be recovered any more,
    /// or its state was restored from an older copy (then a StoreEvent is recorded).
    std::optional<OpenDevice> load(const std::string& userId);
    /// Saves the engine's new state. Called before anything that call produced is sent.
    void saveState(const std::string& userId, const std::string& deviceId, const std::string& state);
    /// Forgets the device: its key and its protocol-3 history here.
    void wipe(const std::string& userId);
    /// What this computer noticed about the account's stored device, oldest first. Kept across wipe() and create().
    std::vector<StoreEvent> events(const std::string& userId) const;

    Protection protection() const { return vault_->protection(); }

    /// The id of the vault entry that mirrors the state's counter (for tests).
    static std::string counterId(const std::string& userId, const std::string& deviceId) { return vaultId(userId, deviceId) + "/counter"; }

private:
    struct Counter {
        std::string deviceId;
        crypto::Bytes macKey;
        std::uint64_t value = 0;
    };
    std::filesystem::path folder(const std::string& userId) const;
    std::filesystem::path eventsFile(const std::string& userId) const;
    static std::string vaultId(const std::string& userId, const std::string& deviceId) { return userId + "/" + deviceId; }
    void record(const std::string& userId, StoreEvent event);
    void writeMirror(const std::string& userId, const Counter& counter);

    std::filesystem::path dir_;
    std::shared_ptr<KeyVault> vault_;
    mutable std::mutex mutex_;
    /// The counter of each device this store opened or made, keyed by user.
    std::map<std::string, Counter> counters_;
};

}  // namespace redvault::e2ev3
